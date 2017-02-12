/*
  This file is part of Ingen.
  Copyright 2015-2016 David Robillard <http://drobilla.net/>

  Ingen is free software: you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free
  Software Foundation, either version 3 of the License, or any later version.

  Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for details.

  You should have received a copy of the GNU Affero General Public License
  along with Ingen.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>

#include "ingen/ColorContext.hpp"
#include "ingen/Configuration.hpp"
#include "ingen/Log.hpp"

#include "CompiledGraph.hpp"
#include "Engine.hpp"
#include "GraphImpl.hpp"
#include "ThreadManager.hpp"

namespace Ingen {
namespace Server {

/** Graph contains ambiguous feedback with no delay nodes. */
class FeedbackException : public std::exception {
public:
	FeedbackException(const BlockImpl* node, const BlockImpl* root=NULL)
		: node(node)
		, root(root)
	{}

	const BlockImpl* node;
	const BlockImpl* root;
};

CompiledGraph::CompiledGraph(GraphImpl* graph)
	: _log(graph->engine().log())
	, _path(graph->path())
	, _master(Task::Mode::SEQUENTIAL)
{
	compile_graph(graph);
}

MPtr<CompiledGraph>
CompiledGraph::compile(Raul::Maid& maid, GraphImpl& graph)
{
	try {
		return maid.make_managed<CompiledGraph>(&graph);
	} catch (FeedbackException e) {
		Log& log = graph.engine().log();
		if (e.node && e.root) {
			log.error(fmt("Feedback compiling %1% from %2%\n")
			          % e.node->path() % e.root->path());
		} else {
			log.error(fmt("Feedback compiling %1%\n")
			          % e.node->path());
		}
		return MPtr<CompiledGraph>();
	}
}

void
CompiledGraph::compile_set(const std::set<BlockImpl*>& blocks,
                           Task&                       task,
                           std::set<BlockImpl*>&       k)
{
	// Keep compiling working set until all nodes are visited
	for (BlockImpl* block : blocks)  {
		// Each block is the start of a new sequential task
		Task seq(Task::Mode::SEQUENTIAL);
		compile_block(block, seq, k);
		task.push_back(seq);
	}
}

void
CompiledGraph::compile_graph(GraphImpl* graph)
{
	ThreadManager::assert_thread(THREAD_PRE_PROCESS);

	// Start with sink nodes (no outputs, or connected only to graph outputs)
	std::set<BlockImpl*> blocks;
	for (auto& b : graph->blocks()) {
		// Mark all blocks as unvisited initially
		b.set_mark(BlockImpl::Mark::UNVISITED);

		if (b.providers().empty()) {
			// Block has no dependencies, add to initial working set
			blocks.insert(&b);
		}
	}

	// Compile initial working set into master task
	Task                 start(Task::Mode::PARALLEL);
	std::set<BlockImpl*> next;
	compile_set(blocks, start, next);
	_master.push_back(start);

	// Keep compiling working set until all connected nodes are visited
	while (!next.empty()) {
		blocks.clear();
		// The working set is a parallel task...
		Task par(Task::Mode::PARALLEL);
		for (BlockImpl* block : next)  {
			// ... where each block is the start of a new sequential task
			Task seq(Task::Mode::SEQUENTIAL);
			compile_block(block, seq, blocks);
			par.push_back(seq);
		}
		_master.push_back(par);
		next = blocks;
	}

	// Compile any nodes that weren't reached (disconnected cycles)
	for (auto& b : graph->blocks()) {
		if (b.get_mark() == BlockImpl::Mark::UNVISITED) {
			compile_block(&b, _master, next);
		}
	}

	_master.simplify();

	if (graph->engine().world()->conf().option("trace").get<int32_t>()) {
		dump([this](const std::string& msg) {
				ColorContext ctx(stderr, ColorContext::Color::YELLOW);
				fwrite(msg.c_str(), 1, msg.size(), stderr);
			});
	}
}

/** Throw a FeedbackException iff `dependant` has `root` as a dependency. */
static void
check_feedback(const BlockImpl* root, BlockImpl* dependant)
{
	if (dependant == root) {
		throw FeedbackException(root);
	}

	for (auto& d : dependant->dependants()) {
		const BlockImpl::Mark mark = d->get_mark();
		switch (mark) {
		case BlockImpl::Mark::UNVISITED:
			d->set_mark(BlockImpl::Mark::VISITING);
			check_feedback(root, d);
			break;
		case BlockImpl::Mark::VISITING:
			throw FeedbackException(d, root);
		case BlockImpl::Mark::VISITED:
			break;
		}
		d->set_mark(mark);
	}
}

void
CompiledGraph::compile_dependant(const BlockImpl*      root,
                                 BlockImpl*            block,
                                 Task&                 task,
                                 std::set<BlockImpl*>& k)
{
	if (block->providers().size() > 1) {
		/* Dependant has other providers, so this is the start of a sequential task.
		   Add dependant to future working set and stop traversal. */
		check_feedback(root, block);
		k.insert(block);
	} else {
		// Dependant has only this provider, add here
		if (task.mode() == Task::Mode::PARALLEL) {
			// Inside a parallel task, compile into a new sequential child
			Task seq(Task::Mode::SEQUENTIAL);
			compile_block(block, seq, k);
			task.push_back(seq);
		} else {
			// Append to given sequential task
			compile_block(block, task, k);
		}
	}
}

void
CompiledGraph::compile_block(BlockImpl* n, Task& task, std::set<BlockImpl*>& k)
{
	switch (n->get_mark()) {
	case BlockImpl::Mark::UNVISITED:
		n->set_mark(BlockImpl::Mark::VISITING);

		// Execute this task before the dependants to follow
		task.push_back(Task(Task::Mode::SINGLE, n));

		if (n->dependants().size() < 2) {
			// Single dependant, append to this sequential task
			for (auto& d : n->dependants()) {
				compile_dependant(n, d, task, k);
			}
		} else {
			// Multiple dependants, create a new parallel task
			Task par(Task::Mode::PARALLEL);
			for (auto& d : n->dependants()) {
				compile_dependant(n, d, par, k);
			}
			task.push_back(par);
		}
		n->set_mark(BlockImpl::Mark::VISITED);
		break;

	case BlockImpl::Mark::VISITING:
		throw FeedbackException(n);

	case BlockImpl::Mark::VISITED:
		break;
	}
}

void
CompiledGraph::run(RunContext& context)
{
	_master.run(context);
}

void
CompiledGraph::dump(std::function<void (const std::string&)> sink) const
{
	sink("(compiled-graph ");
	sink(_path);
	_master.dump(sink, 2, false);
	sink(")\n");
}

} // namespace Server
} // namespace Ingen

#include "BlockPipe.h"
#include "../Messages/TransactionKernelMessage.h"
#include "../ConnectionManager.h"

#include <Common/Util/ThreadUtil.h>
#include <Infrastructure/ThreadManager.h>
#include <Infrastructure/Logger.h>
#include <BlockChain/BlockChainServer.h>

BlockPipe::BlockPipe(const Config& config, ConnectionManagerPtr pConnectionManager, IBlockChainServerPtr pBlockChainServer)
	: m_config(config), m_pConnectionManager(pConnectionManager), m_pBlockChainServer(pBlockChainServer), m_terminate(false)
{
}

BlockPipe::~BlockPipe()
{
	m_terminate = true;

	ThreadUtil::Join(m_blockThread);
	ThreadUtil::Join(m_processThread);
}

std::shared_ptr<BlockPipe> BlockPipe::Create(const Config& config, ConnectionManagerPtr pConnectionManager, IBlockChainServerPtr pBlockChainServer)
{
	std::shared_ptr<BlockPipe> pBlockPipe = std::shared_ptr<BlockPipe>(new BlockPipe(config, pConnectionManager, pBlockChainServer));
	pBlockPipe->m_blockThread = std::thread(Thread_ProcessNewBlocks, std::ref(*pBlockPipe.get()));
	pBlockPipe->m_processThread = std::thread(Thread_PostProcessBlocks, std::ref(*pBlockPipe.get()));

	return pBlockPipe;
}

void BlockPipe::Thread_ProcessNewBlocks(BlockPipe& pipeline)
{
	ThreadManagerAPI::SetCurrentThreadName("BLOCK_PREPROCESS_PIPE");
	LOG_TRACE("BEGIN");

	while (!pipeline.m_terminate)
	{
		std::vector<BlockEntry> blocksToProcess = pipeline.m_blocksToProcess.copy_front(8); // TODO: Use number of CPU threads.
		if (!blocksToProcess.empty())
		{
			if (blocksToProcess.size() == 1)
			{
				ProcessNewBlock(pipeline, blocksToProcess.front());
			}
			else
			{
				std::vector<std::thread> tasks;
				for (const BlockEntry& blockEntry : blocksToProcess)
				{
					tasks.push_back(std::thread([&pipeline, blockEntry] { ProcessNewBlock(pipeline, blockEntry); }));
				}

				ThreadUtil::JoinAll(tasks);
			}

			pipeline.m_blocksToProcess.pop_front(blocksToProcess.size());
		}
		else
		{
			ThreadUtil::SleepFor(std::chrono::milliseconds(5), pipeline.m_terminate);
		}
	}

	LOG_TRACE("END");
}

void BlockPipe::ProcessNewBlock(BlockPipe& pipeline, const BlockEntry& blockEntry)
{
	try
	{
		const EBlockChainStatus status = pipeline.m_pBlockChainServer->AddBlock(blockEntry.block);
		if (status == EBlockChainStatus::INVALID)
		{
			pipeline.m_pConnectionManager->BanConnection(blockEntry.connectionId, EBanReason::BadBlock);
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR_F("Exception ({}) caught while attempting to add block {}.", e.what(), blockEntry.block);
		pipeline.m_pConnectionManager->BanConnection(blockEntry.connectionId, EBanReason::BadBlock);
	}
}

void BlockPipe::Thread_PostProcessBlocks(BlockPipe& pipeline)
{
	ThreadManagerAPI::SetCurrentThreadName("BLOCK_POSTPROCESS_PIPE");
	LOG_TRACE("BEGIN");

	while (!pipeline.m_terminate)
	{
		if (!pipeline.m_pBlockChainServer->ProcessNextOrphanBlock())
		{
			ThreadUtil::SleepFor(std::chrono::milliseconds(5), pipeline.m_terminate);
		}
	}

	LOG_TRACE("END");
}

bool BlockPipe::AddBlockToProcess(const uint64_t connectionId, const FullBlock& block)
{
	std::function<bool(const BlockEntry&, const BlockEntry&)> comparator = [](const BlockEntry& blockEntry1, const BlockEntry& blockEntry2)
	{
		return blockEntry1.block.GetHash() == blockEntry2.block.GetHash();
	};

	return m_blocksToProcess.push_back_unique(BlockEntry(connectionId, block), comparator);
}

bool BlockPipe::IsProcessingBlock(const Hash& hash) const
{
	std::function<bool(const BlockEntry&, const Hash&)> comparator = [](const BlockEntry& blockEntry, const Hash& hash)
	{
		return blockEntry.block.GetHash() == hash;
	};

	return m_blocksToProcess.contains<Hash>(hash, comparator);
}
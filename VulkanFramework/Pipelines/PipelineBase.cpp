// Author : Jihong Shin (snowapril)

#include <VulkanFramework/pch.h>
#include <VulkanFramework/Device.h>
#include <VulkanFramework/Pipelines/PipelineBase.h>
#include <VulkanFramework/Pipelines/PipelineConfig.h>
#include <fstream>
#include <cassert>

namespace vfs
{
	PipelineBase::PipelineBase(std::shared_ptr<Device> device)
	{
		assert(initialize(device));
	}

	PipelineBase::~PipelineBase()
	{
		destroyPipeline();
	}

	void PipelineBase::destroyPipeline()
	{
		const VkDevice device = _device->getDeviceHandle();
		for (VkPipelineShaderStageCreateInfo& shaderStage : _shaderStages)
		{
			if (shaderStage.module != VK_NULL_HANDLE)
			{
				vkDestroyShaderModule(device, shaderStage.module, nullptr);
			}
		}
		_shaderStages.clear();

		if (_pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, _pipeline, nullptr);
			_pipeline = VK_NULL_HANDLE;
		}

		_device.reset();
	}

	bool PipelineBase::initialize(std::shared_ptr<Device> device)
	{
		_device = device;
		return true;
	}

	bool PipelineBase::createPipeline(const PipelineConfig* pipelineConfig)
	{
		return initializePipeline(pipelineConfig, _shaderStages);
	}

	void PipelineBase::bindPipeline(VkCommandBuffer commandBuffer)
	{
		assert(_pipeline != VK_NULL_HANDLE);
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
	}

	void PipelineBase::attachShaderModule(VkShaderStageFlagBits stage, const char* shaderPath,
										  const VkSpecializationInfo* specialInfo)
	{
		std::vector<char> tempData;
		assert(ReadSpirvShaderFile(shaderPath, &tempData)); // snowapril : SpirV file must exists in proudct

		VkPipelineShaderStageCreateInfo pipelineShaderStageInfo = {};
		pipelineShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipelineShaderStageInfo.pNext = nullptr;
		pipelineShaderStageInfo.stage = stage;
		pipelineShaderStageInfo.module = CreateShaderModule(_device->getDeviceHandle(), tempData);
		pipelineShaderStageInfo.pName = "main";
		pipelineShaderStageInfo.pSpecializationInfo = specialInfo;
		_shaderStages.emplace_back(std::move(pipelineShaderStageInfo));
	}

	VkShaderModule PipelineBase::CreateShaderModule(VkDevice device, const std::vector<char>& shaderData)
	{
		VkShaderModuleCreateInfo shaderModuleInfo = {};
		shaderModuleInfo.sType		= VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shaderModuleInfo.pNext		= nullptr;
		shaderModuleInfo.codeSize	= shaderData.size() * sizeof(char);
		shaderModuleInfo.pCode		= reinterpret_cast<const uint32_t*>(shaderData.data());
		
		VkShaderModule shaderModule;
		if (vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS)
		{
			return VK_NULL_HANDLE;
		}
		else
		{
			return shaderModule;
		}
	}

	bool PipelineBase::ReadSpirvShaderFile(const char* filePath, std::vector<char>* retData)
	{
		std::ifstream binaryFile(filePath, std::ios::ate | std::ios::binary);
		
		if (!binaryFile.is_open())
		{
			return false;
		}

		const size_t fileSize = static_cast<size_t>(binaryFile.tellg());
		retData->resize(fileSize);

		binaryFile.seekg(0);
		binaryFile.read(retData->data(), fileSize);

		binaryFile.close();
		return true;
	}
}
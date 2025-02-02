// Author : Jihong Shin (snowapril)

#include <pch.h>
#include <Common/Utils.h>
#include <Common/CPUTimer.h>
#include <Common/Logger.h>
#include <VulkanFramework/Device.h>
#include <VulkanFramework/Buffers/Buffer.h>
#include <VulkanFramework/Commands/CommandPool.h>
#include <VulkanFramework/Commands/CommandBuffer.h>
#include <VulkanFramework/Images/Image.h>
#include <VulkanFramework/Images/ImageView.h>
#include <VulkanFramework/Images/Sampler.h>
#include <VulkanFramework/Descriptors/DescriptorPool.h>
#include <VulkanFramework/Descriptors/DescriptorSet.h>
#include <VulkanFramework/Descriptors/DescriptorSetLayout.h>
#include <VulkanFramework/Pipelines/PipelineLayout.h>
#include <VulkanFramework/Queue.h>
#include <VulkanFramework/Sync/Fence.h>
#include <Shaders/gltf.glsl>
#include <GLTFScene.h>
#include <imgui/imgui.h>

namespace vfs
{
	GLTFScene::GLTFScene(DevicePtr device, const char* scenePath, 
						 const QueuePtr& queue, VertexFormat format)
	{
		assert(initialize(device, scenePath, queue, format));
	}

	GLTFScene::~GLTFScene()
	{
		// Do nothing
	}

	bool GLTFScene::initialize(DevicePtr device, const char* scenePath, const QueuePtr& loaderQueue, VertexFormat format)
	{
		_device		= device;
		_queue		= loaderQueue;
		_format		= format;
		_debugUtil	= DebugUtils(_device);
		
		CPUTimer timer;

		if (!loadScene(scenePath, format))
		{
			VFS_ERROR << "Cannot find scene file or not a valid gltf format";
			return false;
		}
		
		char markerBuffer[128];
		// Create buffers for vertices
		// 0. Position Buffer
		_vertexBuffers.push_back(std::make_shared<Buffer>(_device->getMemoryAllocator(),
								 _positions.size() * VertexHelper::GetNumBytes(VertexFormat::Position3),
								 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								 VMA_MEMORY_USAGE_GPU_ONLY));
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Position Buffer");
		_debugUtil.setObjectName(_vertexBuffers[0]->getBufferHandle(), markerBuffer);

		// 1. Normal Buffer
		_vertexBuffers.push_back(std::make_shared<Buffer>(_device->getMemoryAllocator(),
								 _normals.size() * VertexHelper::GetNumBytes(VertexFormat::Normal3),
								 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								 VMA_MEMORY_USAGE_GPU_ONLY));
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Normal Buffer");
		_debugUtil.setObjectName(_vertexBuffers[1]->getBufferHandle(), markerBuffer);

		// 2. TexCoord Buffer
		_vertexBuffers.push_back(std::make_shared<Buffer>(_device->getMemoryAllocator(),
								 _texCoords.size() * VertexHelper::GetNumBytes(VertexFormat::TexCoord2),
								 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								 VMA_MEMORY_USAGE_GPU_ONLY));
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "TexCoord Buffer");
		_debugUtil.setObjectName(_vertexBuffers[2]->getBufferHandle(), markerBuffer);

		// 3. Tangent Buffer
		// Tangent vector (x, y, z) and handedness (w)
		_vertexBuffers.push_back(std::make_shared<Buffer>(_device->getMemoryAllocator(),
								 _tangents.size() * VertexHelper::GetNumBytes(VertexFormat::Tangent4),
								 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								 VMA_MEMORY_USAGE_GPU_ONLY));
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Tangent Buffer");
		_debugUtil.setObjectName(_vertexBuffers[3]->getBufferHandle(), markerBuffer);

		// Create buffers for indices
		_indexBuffer = std::make_shared<Buffer>(_device->getMemoryAllocator(),
												_indices.size() * sizeof(uint32_t), 
												VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
												VMA_MEMORY_USAGE_GPU_ONLY);
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Index Buffer");
		_debugUtil.setObjectName(_indexBuffer->getBufferHandle(), markerBuffer);

		// Create shader storage buffer object for matrices of scene nodes
		size_t numMatrices{ 0 };
		for (const GLTFNode& node : _sceneNodes)
		{
			numMatrices += static_cast<uint32_t>(node.primMeshes.empty() == false);
		}
		
		_matrixBuffer = std::make_shared<Buffer>(_device->getMemoryAllocator(), 
												 numMatrices * sizeof(glm::mat4) * 2,
												 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
												 VMA_MEMORY_USAGE_GPU_ONLY);
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Matrix Buffer");
		_debugUtil.setObjectName(_matrixBuffer->getBufferHandle(), markerBuffer);

		_materialBuffer = std::make_shared<Buffer>(_device->getMemoryAllocator(), 
												   _sceneMaterials.size() * sizeof(GltfShadeMaterial),
												   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
												   VMA_MEMORY_USAGE_GPU_ONLY);
		snprintf(markerBuffer, sizeof(markerBuffer), "%s(%s)", scenePath, "Material Buffer");
		_debugUtil.setObjectName(_materialBuffer->getBufferHandle(), markerBuffer);

		uploadBuffer();
		uploadImage();
		uploadMaterialBuffer();
		uploadMatrixBuffer();

		// After uploading all required vertex data and images We can release them to free
		releaseSourceData();

		VFS_INFO << scenePath << " scene loaded ( " << timer.elapsedSeconds() << " second )";
		return true;
	}

	bool GLTFScene::uploadBuffer(void)
	{
		VmaAllocator allocator = _device->getMemoryAllocator();

		const uint32_t positionBufSize = static_cast<uint32_t>(_positions.size()) * 
										 VertexHelper::GetNumBytes(VertexFormat::Position3);
		Buffer stagingPosition(allocator, positionBufSize,
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							   VMA_MEMORY_USAGE_CPU_ONLY);
		stagingPosition.uploadData(_positions.data(), positionBufSize);
		
		const uint32_t normalBufSize = static_cast<uint32_t>(_normals.size()) *
										 VertexHelper::GetNumBytes(VertexFormat::Normal3);
		Buffer stagingNormal(allocator, normalBufSize,
							 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							 VMA_MEMORY_USAGE_CPU_ONLY);
		stagingNormal.uploadData(_normals.data(), normalBufSize);

		const uint32_t texCoordBufSize = static_cast<uint32_t>(_texCoords.size()) *
										 VertexHelper::GetNumBytes(VertexFormat::TexCoord2);
		Buffer stagingTexCoord(allocator, texCoordBufSize,
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							   VMA_MEMORY_USAGE_CPU_ONLY);
		stagingTexCoord.uploadData(_texCoords.data(), texCoordBufSize);

		const uint32_t tangentBufSize = static_cast<uint32_t>(_tangents.size()) *
										VertexHelper::GetNumBytes(VertexFormat::Tangent4);
		Buffer stagingTangent(allocator, tangentBufSize,
							  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							  VMA_MEMORY_USAGE_CPU_ONLY);
		stagingTangent.uploadData(_tangents.data(), tangentBufSize);

		const uint32_t indicesBufSize = static_cast<uint32_t>(_indices.size()) * sizeof(uint32_t);
		Buffer stagingIndices(allocator, indicesBufSize,
							  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							  VMA_MEMORY_USAGE_CPU_ONLY);
		stagingIndices.uploadData(_indices.data(), indicesBufSize);

		CommandPool loaderCmdPool(_device, _queue, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
													    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);

		CommandBuffer cmdBuffer(loaderCmdPool.allocateCommandBuffer());
		{
			cmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			cmdBuffer.copyBuffer(&stagingPosition,	_vertexBuffers[0], { { 0, 0, positionBufSize} });
			cmdBuffer.copyBuffer(&stagingNormal,	_vertexBuffers[1], { { 0, 0, normalBufSize  } });
			cmdBuffer.copyBuffer(&stagingTexCoord,	_vertexBuffers[2], { { 0, 0, texCoordBufSize} });
			cmdBuffer.copyBuffer(&stagingTangent,	_vertexBuffers[3], { { 0, 0, tangentBufSize } });
			cmdBuffer.copyBuffer(&stagingIndices,		 _indexBuffer, { { 0, 0, indicesBufSize } });

			cmdBuffer.endRecord();

			Fence fence(_device, 1, 0);
			_queue->submitCmdBuffer({ cmdBuffer }, &fence);
			if (!fence.waitForAllFences(UINT64_MAX))
			{
				return false;
			}
		}

		return true;
	}

	bool GLTFScene::uploadImage(void)
	{
		CommandPool loaderCmdPool(_device, _queue, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
														VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
		std::vector<Buffer> stagingBuffers(_images.size());
		for (size_t i = 0; i < _images.size(); ++i)
		{
			CommandBuffer coypCmdBuffer(loaderCmdPool.allocateCommandBuffer());
			coypCmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			GLTFImage& image = _images[i];

			const uint32_t mipLevels = static_cast<uint32_t>(std::floor(
				std::log2(vfs::max(image.width, image.height))
			)) + 1;

			VkImageCreateInfo imageInfo = Image::GetDefaultImageCreateInfo();
			imageInfo.extent		= { static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), 1 };
			imageInfo.format		= VK_FORMAT_R8G8B8A8_UNORM;
			imageInfo.usage			= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			imageInfo.imageType		= VK_IMAGE_TYPE_2D;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageInfo.mipLevels		= mipLevels;
			ImagePtr imageBuffer = std::make_shared<Image>(_device->getMemoryAllocator(), VMA_MEMORY_USAGE_GPU_ONLY, imageInfo);

			stagingBuffers[i].initialize(_device->getMemoryAllocator(), image.width * image.height * 4,
										 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
			stagingBuffers[i].uploadData(image.data.data(), image.width * image.height * 4);

			coypCmdBuffer.pipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, {}, {}, 
				{ imageBuffer->generateMemoryBarrier(0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) }
			);

			VkBufferImageCopy bufferImageCopy = {};
			bufferImageCopy.bufferOffset					= 0;
			bufferImageCopy.bufferRowLength					= 0;
			bufferImageCopy.bufferImageHeight				= 0;
			bufferImageCopy.imageSubresource.aspectMask		= VK_IMAGE_ASPECT_COLOR_BIT;
			bufferImageCopy.imageSubresource.baseArrayLayer = 0;
			bufferImageCopy.imageSubresource.layerCount		= 1;
			bufferImageCopy.imageSubresource.mipLevel		= 0;
			bufferImageCopy.imageOffset						= { 0, 0, 0 };
			bufferImageCopy.imageExtent						= imageBuffer->getDimension();

			coypCmdBuffer.copyBufferToImage(&stagingBuffers[i], imageBuffer, { bufferImageCopy });

			coypCmdBuffer.pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, {}, {},
				{ imageBuffer->generateMemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) }
			);

			coypCmdBuffer.endRecord();

			Fence copyCmdFence(_device, 1, 0);
			_queue->submitCmdBuffer({ coypCmdBuffer }, &copyCmdFence);
			copyCmdFence.waitForAllFences(UINT64_MAX);

			CommandBuffer blitCmdBuffer(loaderCmdPool.allocateCommandBuffer());
			blitCmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			// Mipmap generation
			VkImageMemoryBarrier mipmapBarrier = {};
			mipmapBarrier.sType								= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			mipmapBarrier.pNext								= nullptr;
			mipmapBarrier.image								= imageBuffer->getImageHandle();
			mipmapBarrier.srcQueueFamilyIndex				= VK_QUEUE_FAMILY_IGNORED;
			mipmapBarrier.dstQueueFamilyIndex				= VK_QUEUE_FAMILY_IGNORED;
			mipmapBarrier.subresourceRange.aspectMask		= VK_IMAGE_ASPECT_COLOR_BIT;
			mipmapBarrier.subresourceRange.baseArrayLayer	= 0;
			mipmapBarrier.subresourceRange.layerCount		= 1;
			mipmapBarrier.subresourceRange.levelCount		= 1;

			int32_t mipWidth  = static_cast<int32_t>(image.width);
			int32_t mipHeight = static_cast<int32_t>(image.height);
			
			for (uint32_t mip = 1; mip < mipLevels; ++mip)
			{
				mipmapBarrier.subresourceRange.baseMipLevel = mip;
				mipmapBarrier.oldLayout						= VK_IMAGE_LAYOUT_UNDEFINED;
				mipmapBarrier.newLayout						= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				mipmapBarrier.srcAccessMask					= 0;
				mipmapBarrier.dstAccessMask					= VK_ACCESS_TRANSFER_WRITE_BIT;

				blitCmdBuffer.pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, {}, {}, { mipmapBarrier });

				VkImageBlit blit{};
				// Blit source
				blit.srcOffsets[0] = { 0, 0, 0 };
				blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
				blit.srcSubresource.aspectMask		= VK_IMAGE_ASPECT_COLOR_BIT;
				blit.srcSubresource.mipLevel		= mip - 1;
				blit.srcSubresource.baseArrayLayer	= 0;
				blit.srcSubresource.layerCount		= 1;
				// Blit destination
				blit.dstOffsets[0] = { 0, 0, 0 };
				blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth >> 1 : 1, mipHeight > 1 ? mipHeight >> 1 : 1, 1 };
				blit.dstSubresource.aspectMask		= VK_IMAGE_ASPECT_COLOR_BIT;
				blit.dstSubresource.mipLevel		= mip;
				blit.dstSubresource.baseArrayLayer	= 0;
				blit.dstSubresource.layerCount		= 1;

				blitCmdBuffer.blitImage(imageBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					imageBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, { blit }, VK_FILTER_LINEAR);

				mipmapBarrier.oldLayout		= VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				mipmapBarrier.newLayout		= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				mipmapBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				mipmapBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

				blitCmdBuffer.pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, {}, {}, { mipmapBarrier });

				if (mipWidth > 1)	mipWidth >>= 1;
				if (mipHeight > 1)	mipHeight >>= 1;
			}

			blitCmdBuffer.endRecord();

			Fence blitCmdFence(_device, 1, 0);
			_queue->submitCmdBuffer({ blitCmdBuffer }, &blitCmdFence);
			blitCmdFence.waitForAllFences(UINT64_MAX);

			CommandBuffer finalTransitionCmdBuffer(loaderCmdPool.allocateCommandBuffer());
			finalTransitionCmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			mipmapBarrier.subresourceRange.baseMipLevel = 0;
			mipmapBarrier.subresourceRange.levelCount	= mipLevels;
			mipmapBarrier.oldLayout		= VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			mipmapBarrier.newLayout		= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			mipmapBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			mipmapBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			finalTransitionCmdBuffer.pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, {}, {}, { mipmapBarrier });

			finalTransitionCmdBuffer.endRecord();

			Fence finalTransitionFence(_device, 1, 0);
			_queue->submitCmdBuffer({ finalTransitionCmdBuffer }, &finalTransitionFence);
			finalTransitionFence.waitForAllFences(UINT64_MAX);

			_textureImages.emplace_back(imageBuffer);
			_textureImageViews.emplace_back(std::make_shared<ImageView>(_device, imageBuffer, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels));
			_textureSamplers.emplace_back(std::make_shared<Sampler>(_device, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_FILTER_LINEAR, 0.0f));
		}
		
		return true;
	}

	bool GLTFScene::uploadMaterialBuffer(void)
	{
		const VmaAllocator allocator = _device->getMemoryAllocator();
		
		// Create shader storage buffer object for materials and fill it
		std::vector<GltfShadeMaterial> materials;
		materials.reserve(_sceneMaterials.size());
		for (const GLTFMaterial& material : _sceneMaterials)
		{
			materials.push_back({   material.baseColorFactor,
									material.baseColorTexture,
									material.metallicFactor,
									material.roughnessFactor,
									material.metallicRoughnessTexture,
									material.emissiveTexture,
									material.alphaMode,
									material.alphaCutoff,
									material.doubleSided,
									material.emissiveFactor,
									material.normalTexture,
									material.normalTextureScale,
									material.occlusionTexture,
									material.occlusionTextureStrength } );
		}

		const uint32_t materialBufSize = static_cast<uint32_t>(materials.size()) * sizeof(GltfShadeMaterial);
		Buffer stagingMaterial(allocator, materialBufSize,
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							   VMA_MEMORY_USAGE_CPU_ONLY);
		stagingMaterial.uploadData(materials.data(), materialBufSize);

		CommandPool loaderCmdPool(_device, _queue, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
													    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);

		CommandBuffer cmdBuffer(loaderCmdPool.allocateCommandBuffer());
		{
			cmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			cmdBuffer.copyBuffer(&stagingMaterial, _materialBuffer, { { 0, 0, materialBufSize } });

			cmdBuffer.endRecord();

			Fence fence(_device, 1, 0);
			_queue->submitCmdBuffer({ cmdBuffer }, &fence);
			if (!fence.waitForAllFences(UINT64_MAX))
			{
				return false;
			}
		}

		return true;
	}

	bool GLTFScene::uploadMatrixBuffer(void)
	{
		const VmaAllocator allocator = _device->getMemoryAllocator();

		std::vector<std::pair<glm::mat4, glm::mat4>> matrixBuf;
		matrixBuf.reserve(_sceneNodes.size());

		for (const GLTFNode& node : _sceneNodes)
		{
			if (!node.primMeshes.empty())
			{
				matrixBuf.emplace_back(node.world, glm::transpose(glm::inverse(node.world)));
			}
		}

		const uint32_t matrixBufSize = static_cast<uint32_t>(matrixBuf.size() * sizeof(glm::mat4) * 2);
		Buffer stagingMatrix(allocator, matrixBufSize,
							 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							 VMA_MEMORY_USAGE_CPU_ONLY);
		stagingMatrix.uploadData(matrixBuf.data(), matrixBufSize);

		CommandPool loaderCmdPool(_device, _queue, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
													    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);

		CommandBuffer cmdBuffer(loaderCmdPool.allocateCommandBuffer());
		{
			cmdBuffer.beginRecord(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

			cmdBuffer.copyBuffer(&stagingMatrix, _matrixBuffer, { { 0, 0, matrixBufSize } });

			cmdBuffer.endRecord();

			Fence fence(_device, 1, 0);
			_queue->submitCmdBuffer({ cmdBuffer }, &fence);
			if (!fence.waitForAllFences(UINT64_MAX))
			{
				return false;
			}
		}

		return true;
	}

	void GLTFScene::allocateDescriptor(const DescriptorPoolPtr& pool, const DescriptorSetLayoutPtr& layout)
	{
		_descriptorSet = std::make_shared<DescriptorSet>(_device, pool, layout, 1);
		std::vector<VkDescriptorImageInfo> imageInfos(_textureImages.size());
		for (size_t i = 0; i < _textureImages.size(); ++i)
		{
			VkDescriptorImageInfo& imageInfo = imageInfos[i];
			imageInfo.imageLayout	= VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo.sampler		= _textureSamplers[i]->getSamplerHandle();
			imageInfo.imageView		= _textureImageViews[i]->getImageViewHandle();
		}
		_descriptorSet->updateStorageBuffer({ _matrixBuffer }, 0, 1);
		_descriptorSet->updateStorageBuffer({ _materialBuffer }, 1, 1);
		_descriptorSet->updateImage(imageInfos, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	}

	void GLTFScene::cmdDraw(VkCommandBuffer cmdBufferHandle, const PipelineLayoutPtr& pipelineLayout,
							const uint32_t pushConstOffset)
	{
		const VkPipelineLayout layoutHandle = pipelineLayout->getLayoutHandle();
		CommandBuffer cmdBuffer(cmdBufferHandle);

		std::vector<VkDeviceSize> offsets(_vertexBuffers.size(), 0);
		cmdBuffer.bindVertexBuffers(_vertexBuffers, offsets);
		cmdBuffer.bindIndexBuffer(_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		cmdBuffer.bindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layoutHandle, 1, { _descriptorSet }, {});

		DebugUtils::ScopedCmdLabel scope = _debugUtil.scopeLabel(cmdBufferHandle, "Scene Rendering");

		uint32_t instanceIndex = 0;
		uint32_t lastMaterialIndex = UINT32_MAX;
		for (const GLTFNode& sceneNode : _sceneNodes)
		{
			for (uint32_t meshIdx : sceneNode.primMeshes)
			{
				GLTFPrimMesh& primMesh = _scenePrimMeshes[meshIdx];
				if (static_cast<uint32_t>(primMesh.materialIndex) != lastMaterialIndex)
				{
					lastMaterialIndex = primMesh.materialIndex;
				}
		
				uint32_t pushValues[] = { instanceIndex, lastMaterialIndex };
				cmdBuffer.pushConstants(layoutHandle, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					pushConstOffset, sizeof(pushValues), pushValues);
		
				cmdBuffer.drawIndexed(primMesh.indexCount, 1, primMesh.firstIndex, primMesh.vertexOffset, 0);
			}
			++instanceIndex;
		}
	}

	void GLTFScene::drawGUI(void)
	{
		// TODO(snowapril) : upload only modified part of buffer
		std::string transformNode = "Transform" + std::to_string(_sceneMaterials.size());
		std::string materialNode = "Material" + std::to_string(_sceneMaterials.size());
		if (ImGui::TreeNode(transformNode.c_str()))
		{
			bool bModified = false;
			size_t index = 0;
			for (GLTFNode& node : _sceneNodes)
			{
				std::string subTransformNode = "Node" + std::to_string(index++);
				if (ImGui::TreeNode(subTransformNode.c_str()))
				{
					if (!node.primMeshes.empty())
					{
						bModified |= ImGui::SliderFloat3("Translation", glm::value_ptr(node.translation), -10.0f, 10.0f);
						bModified |= ImGui::SliderFloat3("Scale", glm::value_ptr(node.scale), 0.0f, 10.0f);
						bModified |= ImGui::SliderFloat3("Rotation", glm::value_ptr(node.rotation), -10.0f, 10.0f);
						node.world = glm::translate(glm::mat4(1.0f), node.translation) *
							glm::toMat4(node.rotation) *
							glm::scale(glm::mat4(1.0f), node.scale) *
							node.local;
					}
					ImGui::TreePop();
				}
			}
			if (bModified)
			{
				uploadMatrixBuffer();
			}
			ImGui::TreePop();
		}
		if (ImGui::TreeNode(materialNode.c_str()))
		{
			bool bModified = false;
			for (GLTFMaterial& material : _sceneMaterials)
			{
				std::string treeName = "Material" + material.name;
				if (ImGui::TreeNode(treeName.c_str()))
				{
					bModified |= ImGui::SliderFloat3("BaseColorFactor", glm::value_ptr(material.baseColorFactor),	0.0f, 1.0f);
					bModified |= ImGui::SliderFloat3("EmissiveFactor",	glm::value_ptr(material.emissiveFactor),	0.0f, 1.0f);
					bModified |= ImGui::SliderFloat("MetallicFactor",	&material.metallicFactor,	0.0f, 1.0f);
					bModified |= ImGui::SliderFloat("RoughnessFactor",	&material.roughnessFactor,	0.0f, 1.0f);
					ImGui::TreePop();
				}
			}
			if (bModified)
			{
				uploadMaterialBuffer();
			}
			ImGui::TreePop();
		}
	}
}
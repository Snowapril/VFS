// Author : Jihong Shin (snowapril)

#if !defined(RENDER_ENGINE_SAMPLER_H)
#define RENDER_ENGINE_SAMPLER_H

#include <Common/pch.h>

namespace vfs
{
	class Sampler : NonCopyable
	{
	public:
		explicit Sampler() = default;
		explicit Sampler(DevicePtr device, VkSamplerAddressMode sampleMode, VkFilter filter, float mipmapLevel);
				~Sampler();

	public:
		void destroySamplerHandler (void);
		bool initialize			 (DevicePtr device, VkSamplerAddressMode sampleMode, VkFilter filter, float maxLod);

		inline VkSampler getSamplerHandle(void) const
		{
			return _samplerHandle;
		}

	private:
		DevicePtr	_device				{ nullptr };
		VkSampler	_samplerHandle{ VK_NULL_HANDLE };
	};
}

#endif
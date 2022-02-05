// Author : Jihong Shin (snowapril)

#if !defined(VFS_CPU_TIMER_H)
#define VFS_CPU_TIMER_H

#include <chrono>

namespace vfs
{
	class CPUTimer
	{
	public:
		CPUTimer() noexcept;

	public:
		void	reset				(void) noexcept;
		float	elapsedSeconds		(void) const noexcept;
		float	elapsedMilliSeconds	(void) const noexcept;

	private:
		std::chrono::steady_clock::time_point _startTime;
	};
};

#endif
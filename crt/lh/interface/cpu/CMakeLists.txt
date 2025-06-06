add_library(lh_ifcpu cpu_common.c)
set_target_properties(lh_ifcpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(EZ_ARCH_ARM)
	add_subdirectory(arm)
	target_link_libraries(lh_ifcpu libcpu_arm)
endif()

if(EZ_ARCH_ARM64)
	add_subdirectory(arm64)
	target_link_libraries(lh_ifcpu libcpu_arm64)
endif()

if(EZ_ARCH_MIPS)
	add_subdirectory(mips)
	target_link_libraries(lh_ifcpu libcpu_mips)
endif()

if(USE_CAPSTONE)
	if(EZ_ARCH_I386 OR EZ_ARCH_AMD64)
		add_subdirectory(intel)
		target_link_libraries(lh_ifcpu libcpu_intel)
	endif()
	target_link_libraries(lh_ifcpu ${CAPSTONE_LIBRARIES})
endif()

install(
	TARGETS lh_ifcpu
	EXPORT ${ezinject_target_export}
)
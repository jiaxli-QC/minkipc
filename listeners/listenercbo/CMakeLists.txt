project(listenercbo C)

# ''Build IDL Headers''.

set (IDLS
	idl/CRegisterListenerCBO
	idl/IClientEnv
	idl/ICredentials
	idl/IIO
	idl/IListenerCBO
	idl/IMemRegion
	idl/IRegisterListenerCBO
)

foreach(IDL ${IDLS})
	foreach(VARIANT "" "--skel" )
		if (VARIANT)
			set(INVOKE _invoke)
		else()
			set(INVOKE "")
		endif()

		set(IDL_OUT ${CMAKE_CURRENT_SOURCE_DIR}/${IDL}${INVOKE}.h)
		set(INPUT ${CMAKE_CURRENT_SOURCE_DIR}/${IDL}.idl)

		add_custom_command(
			OUTPUT  ${IDL_OUT}
			DEPENDS ${INPUT}
			COMMAND ${MINKIDLC_BIN_DIR}/idlc ${INPUT} -o ${IDL_OUT} ${VARIANT}
		)

		list(APPEND IDL_OUT_LIST ${IDL_OUT})
	endforeach()
endforeach()

# A custom target to build the MINK-HEADERS which is always out of date.
add_custom_target(
        ${PROJECT_NAME}_MINKHEADERS ALL
        DEPENDS ${IDL_OUT_LIST}
)

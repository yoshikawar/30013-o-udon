function(hexa_udon_enable_warnings target_name)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      ${target_name}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )
  elseif(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
  endif()
endfunction()

idf_component_register(
    SRCS "main.cpp" "dsp.cpp"
    INCLUDE_DIRS ".")

# The DSP runs per-sample at 32 kHz; let the compiler be aggressive with floats.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -ffast-math
    -fno-math-errno)

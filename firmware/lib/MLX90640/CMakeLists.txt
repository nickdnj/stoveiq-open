idf_component_register(
    SRCS "MLX90640_I2C_Driver.c" "MLX90640_API.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)

# Relax warnings for third-party Melexis library code
set_source_files_properties(MLX90640_API.c PROPERTIES COMPILE_FLAGS "-Wno-error=return-type -Wno-return-type")

set(src_dirs
            LED
            MYIIC
            )

set(include_dirs
            LED
            MYIIC
            )

set(requires
            driver
            esp_common
            )

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)

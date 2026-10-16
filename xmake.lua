-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("F4Online")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("F4Online")
    add_rules("commonlibf4.plugin", {
        name = "F4Online",
        author = "F4 Online",
        description = "F4 Online: co-op multiplayer for Fallout 4"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_syslinks("ws2_32")

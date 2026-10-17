-- set minimum xmake version
set_xmakever("2.8.2")

-- includes
includes("lib/commonlibsse-ng")

-- set project
set_project("BlankMap")
set_version("1.0.0")
set_license("GPL-3.0")

-- set defaults
set_languages("c++23")
set_warnings("allextra")

-- add rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- set policies
set_policy("package.requires_lock", true)

-- targets
target("BlankMap")
    -- add dependencies to target
    add_deps("commonlibsse-ng")

    -- add commonlibsse-ng plugin
    add_rules("commonlibsse-ng.plugin", {
        name = "BlankMap",
        author = "mediocreparadise",
        description = "Blank Map - hides world map markers, recentres the map camera, optional compass hiding",
        email = "github.com/mediocreparadise"
    })

    -- add src files (no precompiled header: none of the sources include pch.h)
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

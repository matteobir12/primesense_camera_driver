-- switch build type:
-- xmake f -m debug
set_project("PS1080UDriver")

set_config("buildir", "build")

add_rules("mode.debug", "mode.release")

target("PS1080UDriver_testing")
    set_kind("binary")
    set_languages("c++17")
    add_files(os.files("src/**.cpp"))
    add_includedirs("include")

    if is_mode("debug") then
        add_cxxflags("-Og", "-g", "-ggdb",  "-Wall", {force = true})
    elseif is_mode("release") then
        add_cxxflags("-O3")
    end

target("PS1080UDriver")
    set_kind("shared")
    set_languages("c++17")
    add_files(os.files("src/**.cpp"))
    add_includedirs("include")
    add_cxxflags("-O3")
    
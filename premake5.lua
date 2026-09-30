-- D110Emu build script (premake 5.0.0-beta8 or newer).
--
--   Windows (Visual Studio 2022):  tools\premake5.exe vs2022   ->  D110Emu.sln, output in bin\<Config>
--   Linux (the makefiles ship too): make -C build/gmake-linux config=release   (after changes: premake5 gmake)
--   macOS (the makefiles ship too): make -C build/gmake-macosx config=release   (after changes: premake5 --os=macosx gmake);
--                                   the window's first build downloads SDL 3 (tools/macos-app.sh)
--
-- The generated Visual Studio files ship alongside the sources, so building on Windows
-- only needs D110Emu.sln. Regenerate them after adding or removing source files.

local isVisualStudio = _ACTION ~= nil and _ACTION:match("^vs") ~= nil
local targetsWindows = os.target() == "windows"
local targetsMac = os.target() == "macosx"
-- Makefile builds get per-OS folders so a native build and a MinGW cross-build can coexist.
local projectDir = "build/" .. (_ACTION or "none") .. (isVisualStudio and "" or "-" .. os.target())
local outputDir = isVisualStudio and "%{cfg.buildcfg}" or "%{cfg.system}-%{cfg.buildcfg}"

workspace "D110Emu"
    location(isVisualStudio and "." or projectDir)
    configurations { "Debug", "Release" }
    if targetsWindows then
        platforms { "x64" }
    end
    startproject "D110Emu"
    language "C++"
    cppdialect "C++17"
    targetdir("bin/" .. outputDir)
    objdir("build/obj/" .. outputDir .. "/%{prj.name}")

    filter "platforms:x64"
        architecture "x86_64"

    filter "system:windows"
        systemversion "latest"
        staticruntime "On"  -- The .exe runs without the Visual C++ redistributable
        defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE" }

    -- macOS 11 is the first for Apple silicon, and the first with CoreMIDI's Universal MIDI Packets (the MIDI input).
    -- miniaudio links Core Audio as programs normally do, instead of loading it when it runs.
    filter "system:macosx"
        systemversion "11.0"
        linkoptions { "-mmacosx-version-min=11.0" }  -- premake gives it to the compiler only
        defines { "MA_NO_RUNTIME_LINKING" }

    filter "toolset:msc*"
        multiprocessorcompile "On"
        buildoptions { "/utf-8" }

    -- The runtime is pinned per configuration: mt32emu is optimised even in Debug, and premake
    -- would otherwise give it the release CRT, which MSVC refuses to link with debug-CRT objects.
    filter "configurations:Debug"
        defines { "_DEBUG" }
        symbols "On"
        runtime "Debug"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"
        symbols "On"
        runtime "Release"

    filter {}

-- Third-party headers are "external" so the app's stricter warning level does not apply to them.
local externalDirs = {
    "mt32emu/src",
    "vendor/imgui",
    "vendor/imgui/backends",
    "vendor/imgui/misc/cpp",
    "vendor/miniaudio",
}

-- Synth engine, MIDI file player and ROM handling: shared by the app and the tools.
local engineFiles = {
    "src/SynthEngine.h", "src/SynthEngine.cpp",
    "src/MultiOutputConverter.h", "src/MultiOutputConverter.cpp",
    "src/MidiPlayer.h", "src/MidiPlayer.cpp",
    "src/Mt32Translator.h", "src/Mt32Translator.cpp",
    "src/Mt32Presets.h", "src/Mt32Presets.cpp",
    "src/D20Rhythm.h", "src/D20Rhythm.cpp",
    "src/SmfFile.h", "src/SmfFile.cpp",
    "src/RomLibrary.h", "src/RomLibrary.cpp",
    "src/RomPlay.h", "src/RomPlay.cpp",
    "src/ReverbKit.h", "src/ReverbKit.cpp",
    "src/ReverbSettingsFile.h", "src/ReverbSettingsFile.cpp",
    "src/Settings.h", "src/Settings.cpp",
    "src/MidiInput.h",
    "src/Platform.h",
    targetsWindows and "src/PlatformWin32.cpp" or "src/PlatformPosix.cpp",
}

-- The tone editors (D110Emu's Tone tab and ToneEditor): the tone's parameters, the editor, the piano.
local toneEditorFiles = {
    "src/ToneModel.h", "src/ToneModel.cpp",
    "src/ToneEditor.h", "src/ToneEditor.cpp",
    "src/Piano.h", "src/Piano.cpp",
    "src/RolandSysex.h",
}

-- ToneEditor for real units, without its window: the app, its link to the unit and what they need.
local toneEditorAppFiles = {
    "src/ToneEditorApp.h", "src/ToneEditorApp.cpp",
    "src/UnitLink.h", "src/UnitLink.cpp",
    "src/UnitSetup.h", "src/UnitSetup.cpp",
    "src/PresetNames.h", "src/PresetNames.cpp",
    "src/Mt32Translator.h", "src/Mt32Translator.cpp",
    "src/Mt32Presets.h", "src/Mt32Presets.cpp",
    "src/MidiInput.h", "src/MidiOutput.h",
    "src/Settings.h", "src/Settings.cpp",
    "src/UiStyle.h", "src/UiStyle.cpp",
    "src/Platform.h",
    targetsWindows and "src/PlatformWin32.cpp" or "src/PlatformPosix.cpp",
}

-- Platform-independent application: UI, audio output, settings, the tone editor.
local appFiles = table.join(toneEditorFiles, {
    "src/AppCore.h", "src/AppCore.cpp",
    "src/App.h", "src/App.cpp",
    "src/UiStyle.h", "src/UiStyle.cpp",
    "src/Lcd.h", "src/Lcd.cpp", "src/LcdSchemes.cpp",
    "src/PresetChoiceTable.h", "src/PresetChoiceTable.cpp",
    "src/PresetNames.h", "src/PresetNames.cpp",
    "src/AudioOutput.h", "src/AudioOutput.cpp",
    "src/Settings.h", "src/Settings.cpp",
    "vendor/miniaudio/miniaudio.h", "vendor/miniaudio/miniaudio.c",
})

-- MT-32 translation for real units (MT32Translator, mt32translate, the tests): the pipe and what it needs.
local translatorFiles = {
    "src/Mt32Translator.h", "src/Mt32Translator.cpp",
    "src/Mt32Presets.h", "src/Mt32Presets.cpp",
    "src/MidiPipe.h", "src/MidiPipe.cpp",
    "src/PresetNames.h", "src/PresetNames.cpp",
    "src/MidiInput.h", "src/MidiOutput.h",
    "src/SmfFile.h", "src/SmfFile.cpp",
    "src/RomLibrary.h", "src/RomLibrary.cpp",
    "src/Settings.h", "src/Settings.cpp",
    "src/Platform.h",
    targetsWindows and "src/PlatformWin32.cpp" or "src/PlatformPosix.cpp",
}

-- MT32Translator without its interface (the window, the terminal version, uisnap): settings, ports, presets, files.
local translatorAppFiles = {
    "src/TranslatorCore.h", "src/TranslatorCore.cpp",
}

-- The MIDI ports where the system has them (the Windows programs list WinMM's themselves): input and output, and what
-- they share.
local linuxMidiFiles = { "src/MidiAlsa.h", "src/MidiInputAlsa.cpp", "src/MidiOutputAlsa.cpp", "src/MidiStreamParser.h" }
local macMidiFiles = { "src/MidiCoreMidi.h", "src/MidiInputCoreMidi.cpp", "src/MidiOutputCoreMidi.cpp", "src/MidiStreamParser.h" }

-- The window's host on Linux and macOS: SDL 3 with SDL's renderer through Dear ImGui's backends.
local sdlHostFiles = {
    "src/HostedApp.h", "src/SdlHost.h", "src/SdlHost.cpp",
    "vendor/imgui/backends/imgui_impl_sdl3.h", "vendor/imgui/backends/imgui_impl_sdl3.cpp",
    "vendor/imgui/backends/imgui_impl_sdlrenderer3.h", "vendor/imgui/backends/imgui_impl_sdlrenderer3.cpp",
}

-- The translator's window (TranslatorApp) and the tone editor's, for their hosts.
local translatorWindowFiles = {
    "src/TranslatorApp.h", "src/TranslatorApp.cpp", "src/UiStyle.h", "src/UiStyle.cpp",
    "src/PresetChoiceTable.h", "src/PresetChoiceTable.cpp",
}

-- The terminal interfaces' screens, keys and menus, and what their entry points share.
local terminalFiles = {
    "src/TextScreen.h", "src/TextScreen.cpp",
    "src/TuiMenus.h", "src/TuiMenus.cpp",
    "src/Terminal.h",
    "src/MidiStreamParser.h",
}

-- Steinberg's VST3 interfaces (MIT; only pluginterfaces is vendored): the IDs and helpers they define.
local vst3SdkFiles = {
    "vendor/vst3sdk/pluginterfaces/base/coreiids.cpp",
    "vendor/vst3sdk/pluginterfaces/base/funknown.cpp",
}

-- miniaudio features the app does not use.
local miniaudioDefines = {
    "MA_NO_DECODING", "MA_NO_ENCODING", "MA_NO_GENERATION",
    "MA_NO_RESOURCE_MANAGER", "MA_NO_NODE_GRAPH", "MA_NO_ENGINE",
}

-- What miniaudio's Core Audio backend links on macOS (MA_NO_RUNTIME_LINKING).
local macAudioFrameworks = { "CoreAudio.framework", "AudioToolbox.framework", "CoreFoundation.framework" }

-- Roland LA synthesis emulation: Munt's mt32emu 2.4.0 with D-110 support.
project "mt32emu"
    location(projectDir)
    kind "StaticLib"
    files { "mt32emu/src/**.h", "mt32emu/src/**.cpp" }
    includedirs { "mt32emu/src" }
    defines { "MT32EMU_WITH_INTERNAL_RESAMPLER" }
    -- Always optimised: an unoptimised synth cannot keep up with real time.
    optimize "Speed"
    filter "toolset:msc*"
        defines { "_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1" }
        runtimechecks "Off"  -- /RTC1 (the Debug default) cannot be combined with /O2
    filter "system:linux"
        pic "On"  -- The VST3 plugin, a shared library, links it too...
        buildoptions { "-fno-semantic-interposition" }  -- ...and nothing replaces its functions: optimised as before
    filter {}

project "imgui"
    location(projectDir)
    kind "StaticLib"
    files {
        "vendor/imgui/*.h", "vendor/imgui/*.cpp",
        "vendor/imgui/misc/cpp/imgui_stdlib.h", "vendor/imgui/misc/cpp/imgui_stdlib.cpp",
    }
    includedirs { "vendor/imgui" }
    filter "system:windows"
        files {
            "vendor/imgui/backends/imgui_impl_win32.h", "vendor/imgui/backends/imgui_impl_win32.cpp",
            "vendor/imgui/backends/imgui_impl_dx11.h", "vendor/imgui/backends/imgui_impl_dx11.cpp",
        }
    filter "system:linux"
        pic "On"  -- The VST3 plugin, a shared library, links it too...
        buildoptions { "-fno-semantic-interposition" }  -- ...and nothing replaces its functions: optimised as before
    filter {}

if targetsWindows then
    -- The emulator: Win32 window, Direct3D 11 rendering, WinMM MIDI input, WASAPI audio.
    project "D110Emu"
        location(projectDir)
        kind "WindowedApp"
        files(engineFiles)
        files(appFiles)
        files { "src/MainWin32.cpp", "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp", "src/MidiInputWin32.cpp" }
        files { "res/D110Emu.rc", "res/D110Emu.ico" }  -- The icon (tools/make_icons.py)
        resincludedirs { "res" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        debugdir "."
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The translation box: MIDI for an MT-32 in, MIDI for a real D-110, D-10 or D-20 out (WinMM).
    project "MT32Translator"
        location(projectDir)
        kind "WindowedApp"
        files(translatorFiles)
        files(translatorAppFiles)
        files(translatorWindowFiles)
        files {
            "src/MainTranslatorWin32.cpp", "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp",
            "src/MidiInputWin32.cpp", "src/MidiOutputWin32.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        links { "mt32emu", "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        debugdir "."
        warnings "Extra"
        filter {}

    -- The realtime tone editor for real units: MIDI out to a D-110, D-10, D-20 or MT-32, its answers back (WinMM).
    project "ToneEditor"
        location(projectDir)
        kind "WindowedApp"
        files(toneEditorFiles)
        files(toneEditorAppFiles)
        files {
            "src/MainToneEditorWin32.cpp", "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp",
            "src/MidiInputWin32.cpp", "src/MidiOutputWin32.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        links { "mt32emu", "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        debugdir "."
        warnings "Extra"
        filter {}

    -- The emulator as a VST2 instrument for DAWs: the host's MIDI in, its audio out, the state in its project.
    -- Built as bin\<Config>\VST2\D110Emu.dll, a folder a DAW can scan.
    project "D110EmuVST"
        location(projectDir)
        kind "SharedLib"
        targetname "D110Emu"
        targetdir("bin/" .. outputDir .. "/VST2")
        files(engineFiles)
        files(appFiles)
        files {
            "src/Vst2.h", "src/VstPlugin.h", "src/VstPlugin.cpp", "src/D110EmuVST.def",
            "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorWin32.cpp",
            "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp", "src/MidiInputNull.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter "action:not vs*"  -- MinGW links the module definition file as an input
            linkoptions { path.getabsolute("src/D110EmuVST.def") }
        filter {}

    -- The emulator as a VST3 instrument: the mix and an output per part (the parts on MULTI play out of their own), the
    -- host's MIDI (notes, SysEx, and controllers and program changes as mapped parameters), the state in its project.
    -- Built as the bundle bin\<Config>\VST3\D110Emu.vst3, which goes in C:\Program Files\Common Files\VST3.
    project "D110EmuVST3"
        location(projectDir)
        kind "SharedLib"
        targetname "D110Emu"
        targetextension ".vst3"
        targetdir("bin/" .. outputDir .. "/VST3/D110Emu.vst3/Contents/x86_64-win")
        implibdir("build/obj/" .. outputDir .. "/%{prj.name}")  -- Not in the bundle
        files(engineFiles)
        files(appFiles)
        files(vst3SdkFiles)
        files {
            "src/Vst3Plugin.cpp", "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorWin32.cpp", "src/Vst2.h",
            "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp", "src/MidiInputNull.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        externalincludedirs { "vendor/vst3sdk" }
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- Captures the D-20's preset rhythm patterns from what it plays on its MIDI OUT (WinMM input).
    project "PatternCapture"
        location(projectDir)
        kind "WindowedApp"
        files {
            "src/PatternCaptureApp.h", "src/PatternCaptureApp.cpp", "src/PatternCapture.h", "src/PatternCapture.cpp",
            "src/D20Rhythm.h", "src/D20Rhythm.cpp", "src/SmfFile.h", "src/SmfFile.cpp",
            "src/Settings.h", "src/Settings.cpp", "src/UiStyle.h", "src/UiStyle.cpp",
            "src/MidiInput.h", "src/MidiInputWin32.cpp", "src/Platform.h", "src/PlatformWin32.cpp",
            "src/MainPatternCaptureWin32.cpp", "src/HostedApp.h", "src/Win32Host.h", "src/Win32Host.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        links { "imgui", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "gdi32", "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        debugdir "."
        warnings "Extra"
        filter {}
end

if os.target() == "linux" then
    -- The emulator's window on Linux: SDL 3 (Wayland or X11) drawing with SDL's renderer, ALSA MIDI input (the ALSA
    -- library loaded when it runs), miniaudio. Building it needs SDL 3's development files: sudo apt install libsdl3-dev.
    project "D110Emu"
        location(projectDir)
        kind "WindowedApp"
        targetname "d110emu"
        files(engineFiles)
        files(appFiles)
        files(sdlHostFiles)
        files(linuxMidiFiles)
        files { "src/MainSdl.cpp" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "SDL3", "pthread", "dl", "m" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The translation box's window on Linux (mt32translator), as the emulator's: SDL 3, ALSA MIDI in and out.
    project "MT32Translator"
        location(projectDir)
        kind "WindowedApp"
        targetname "mt32translator"
        files(translatorFiles)
        files(translatorAppFiles)
        files(translatorWindowFiles)
        files(sdlHostFiles)
        files(linuxMidiFiles)
        files { "src/MainTranslatorSdl.cpp" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        links { "mt32emu", "imgui", "SDL3", "pthread", "dl", "m" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The realtime tone editor's window on Linux (toneeditor), as the emulator's: SDL 3, ALSA MIDI in and out.
    project "ToneEditor"
        location(projectDir)
        kind "WindowedApp"
        targetname "toneeditor"
        files(toneEditorFiles)
        files(toneEditorAppFiles)
        files(sdlHostFiles)
        files(linuxMidiFiles)
        files { "src/MainToneEditorSdl.cpp" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        links { "mt32emu", "imgui", "SDL3", "pthread", "dl", "m" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The emulator as a VST3 instrument on Linux, as on Windows: the bundle bin/linux-<Config>/VST3/D110Emu.vst3 (with
    -- D110Emu.so in Contents/<machine>-linux, the machine as uname -m names it), which goes in ~/.vst3. Its window is an
    -- X11 child of the host's, drawn with OpenGL (GLX) through Dear ImGui's OpenGL3 backend and run by the host's
    -- event loop. Building it needs the X11 and OpenGL development files (libx11-dev and libgl-dev, which libsdl3-dev
    -- brings too).
    project "D110EmuVST3"
        location(projectDir)
        kind "SharedLib"
        targetname "D110Emu"
        targetprefix ""
        targetextension ".so"
        targetdir("bin/" .. outputDir .. "/VST3/D110Emu.vst3/Contents/$(shell uname -m)-linux")
        files(engineFiles)
        files(appFiles)
        files(vst3SdkFiles)
        files {
            "src/Vst3Plugin.cpp", "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorX11.cpp", "src/Vst2.h",
            "src/MidiInputNull.cpp", "src/D110EmuVST3.map",
            "vendor/imgui/backends/imgui_impl_opengl3.h", "vendor/imgui/backends/imgui_impl_opengl3.cpp",
            "vendor/imgui/backends/imgui_impl_opengl3_loader.h",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        externalincludedirs { "vendor/vst3sdk" }
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "X11", "GL", "pthread", "dl", "m" }
        -- Only the VST3 entry points leave the library (src/D110EmuVST3.map), and it brings its own C++ runtime: a host's
        -- may be older, and its copies of anything (Dear ImGui, say) must not meet ours.
        visibility "Hidden"
        linkoptions {
            "-Wl,--version-script=" .. path.getrelative(path.getabsolute(projectDir), path.getabsolute("src/D110EmuVST3.map")),
            "-Wl,--no-undefined", "-static-libstdc++", "-static-libgcc",
        }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}
end

if targetsMac then
    -- premake gives every macOS shared library an install name (-install_name), which only a dynamic library has: the
    -- plugin is a loadable bundle (sharedlibtype "OSXBundle"), so it has none.
    premake.override(premake.tools.clang, "getldflags", function(base, cfg)
        local flags = base(cfg)
        if cfg.sharedlibtype == "OSXBundle" then
            flags = table.filter(flags, function(flag) return not flag:find("-install_name", 1, true) end)
        end
        return flags
    end)

    -- The emulator's window on macOS, as the app bundle bin/macosx-<Config>/D110Emu.app: SDL 3 drawing with SDL's renderer
    -- (Metal), CoreMIDI input, miniaudio (Core Audio). SDL 3 is its official framework, which tools/macos-app.sh
    -- downloads once into build/sdl3-macos; after linking, it puts that in the app with the Info.plist and signs the app,
    -- so that the app runs on Macs without SDL (macOS 11 and later).
    local root = path.getrelative(path.getabsolute(projectDir), path.getabsolute("."))
    local sdlDir = path.getrelative(path.getabsolute(projectDir), path.getabsolute("build/sdl3-macos"))
    project "D110Emu"
        location(projectDir)
        kind "WindowedApp"
        targetname "D110Emu"
        targetdir("bin/" .. outputDir .. "/D110Emu.app/Contents/MacOS")
        files(engineFiles)
        files(appFiles)
        files(sdlHostFiles)
        files(macMidiFiles)
        files { "src/MainSdl.cpp", "src/D110Emu.plist", "res/D110Emu.icns" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        buildoptions { "-iframework" .. sdlDir }
        linkoptions { "-F" .. sdlDir, "-Wl,-rpath,@executable_path/../Frameworks" }
        links { "mt32emu", "imgui", "SDL3.framework", "CoreMIDI.framework", "pthread", "m" }
        links(macAudioFrameworks)
        prebuildcommands { "sh " .. root .. "/tools/macos-app.sh sdl3 " .. sdlDir }
        -- The Info.plist (Retina, the app's name and icon, MIDI and SysEx files as documents) and the icon
        -- (tools/make_icons.py).
        postbuildcommands {
            "sh " .. root .. "/tools/macos-app.sh bundle \"$(TARGETDIR)/../..\" " .. root .. "/src/D110Emu.plist " .. sdlDir .. "/SDL3.framework " ..
                root .. "/res/D110Emu.icns",
        }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The translation box and the realtime tone editor as apps, as the emulator's (MT32Translator.app, ToneEditor.app):
    -- SDL 3's framework inside, CoreMIDI in and out.
    local companions = {
        { name = "MT32Translator", main = "src/MainTranslatorSdl.cpp", plist = "src/MT32Translator.plist",
          files = { translatorFiles, translatorAppFiles, translatorWindowFiles } },
        { name = "ToneEditor", main = "src/MainToneEditorSdl.cpp", plist = "src/ToneEditor.plist",
          files = { toneEditorFiles, toneEditorAppFiles } },
    }
    for _, app in ipairs(companions) do
        project(app.name)
            location(projectDir)
            kind "WindowedApp"
            targetname(app.name)
            targetdir("bin/" .. outputDir .. "/" .. app.name .. ".app/Contents/MacOS")
            for _, list in ipairs(app.files) do
                files(list)
            end
            files(sdlHostFiles)
            files(macMidiFiles)
            files { app.main, app.plist }
            includedirs { "src" }
            externalincludedirs(externalDirs)
            buildoptions { "-iframework" .. sdlDir }
            linkoptions { "-F" .. sdlDir, "-Wl,-rpath,@executable_path/../Frameworks" }
            links { "mt32emu", "imgui", "SDL3.framework", "CoreMIDI.framework", "CoreFoundation.framework", "pthread", "m" }
            prebuildcommands { "sh " .. root .. "/tools/macos-app.sh sdl3 " .. sdlDir }
            postbuildcommands {
                "sh " .. root .. "/tools/macos-app.sh bundle \"$(TARGETDIR)/../..\" " .. root .. "/" .. app.plist .. " " .. sdlDir .. "/SDL3.framework",
            }
            warnings "Extra"
            filter "files:vendor/**"
                warnings "Default"
            filter {}
    end

    -- The emulator as a VST3 instrument on macOS, as on Windows and Linux: the bundle bin/macosx-<Config>/VST3/D110Emu.vst3
    -- (its program in Contents/MacOS), which goes in ~/Library/Audio/Plug-Ins/VST3. Its window is an NSView in the host's,
    -- drawn with Metal through Dear ImGui's Metal backend (src/PluginEditorMac.mm). After linking, tools/macos-app.sh
    -- puts the Info.plist in the bundle and signs it.
    project "D110EmuVST3"
        location(projectDir)
        kind "SharedLib"
        sharedlibtype "OSXBundle"  -- A loadable bundle (-bundle), as plugins are
        targetname "D110Emu"
        targetprefix ""
        targetextension ""
        targetdir("bin/" .. outputDir .. "/VST3/D110Emu.vst3/Contents/MacOS")
        files(engineFiles)
        files(appFiles)
        files(vst3SdkFiles)
        files {
            "src/Vst3Plugin.cpp", "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorMac.mm", "src/Vst2.h",
            "src/MidiInputNull.cpp", "src/D110EmuVST3.exp", "src/D110EmuVST3.plist",
            "vendor/imgui/backends/imgui_impl_metal.h", "vendor/imgui/backends/imgui_impl_metal.mm",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        externalincludedirs { "vendor/vst3sdk" }
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "Cocoa.framework", "Metal.framework", "QuartzCore.framework", "Carbon.framework", "pthread", "m" }
        links(macAudioFrameworks)
        -- Only the VST3 entry points leave the bundle (src/D110EmuVST3.exp).
        visibility "Hidden"
        linkoptions { "-Wl,-exported_symbols_list," .. path.getrelative(path.getabsolute(projectDir), path.getabsolute("src/D110EmuVST3.exp")) }
        postbuildcommands { "sh " .. root .. "/tools/macos-app.sh plugin \"$(TARGETDIR)/../..\" " .. root .. "/src/D110EmuVST3.plist" }
        warnings "Extra"
        -- Objective-C++ with automatic reference counting, as Dear ImGui's Metal backend is written.
        filter "files:**.mm"
            buildoptions { "-fobjc-arc" }
        -- Objective-C classes share one namespace in the host's process, where another plugin may have Dear ImGui's
        -- Metal backend too (another version of it, say): the backend's classes get names of their own here.
        filter "files:vendor/imgui/backends/imgui_impl_metal.mm"
            buildoptions {
                "-DMetalBuffer=D110EmuMetalBuffer", "-DFramebufferDescriptor=D110EmuFramebufferDescriptor",
                "-DMetalTexture=D110EmuMetalTexture", "-DMetalContext=D110EmuMetalContext",
            }
        filter "files:vendor/**"
            warnings "Default"
        filter {}

    -- The emulator as an Audio Unit (AUv2) instrument, for Logic Pro, GarageBand, MainStage and the other Audio Unit hosts:
    -- the bundle bin/macosx-<Config>/AU/D110Emu.component (its program in Contents/MacOS), which goes in
    -- ~/Library/Audio/Plug-Ins/Components. Apple's AudioUnitSDK (vendor/AudioUnitSDK) does the Audio Unit's side
    -- (src/AuPlugin.mm); the window is the VST3 plugin's (src/PluginEditorMac.mm). After linking, tools/macos-app.sh puts
    -- the Info.plist (with the unit's AudioComponents entry) in the bundle and signs it.
    local auSdkFiles = {
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUBase.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUBuffer.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUBufferAllocator.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUInputElement.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUMIDIBase.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUOutputElement.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUPlugInDispatch.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/AUScopeElement.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/ComponentBase.cpp",
        "vendor/AudioUnitSDK/src/AudioUnitSDK/MusicDeviceBase.cpp",
    }
    project "D110EmuAU"
        location(projectDir)
        kind "SharedLib"
        sharedlibtype "OSXBundle"  -- A loadable bundle (-bundle), as plugins are
        targetname "D110Emu"
        targetprefix ""
        targetextension ""
        targetdir("bin/" .. outputDir .. "/AU/D110Emu.component/Contents/MacOS")
        files(engineFiles)
        files(appFiles)
        files(auSdkFiles)
        files {
            "src/AuPlugin.mm", "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorMac.mm", "src/Vst2.h",
            "src/MidiInputNull.cpp", "src/D110EmuAU.exp", "src/D110EmuAU.plist",
            "vendor/imgui/backends/imgui_impl_metal.h", "vendor/imgui/backends/imgui_impl_metal.mm",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        externalincludedirs { "vendor/AudioUnitSDK/include" }
        defines(miniaudioDefines)
        links {
            "mt32emu", "imgui", "AudioToolbox.framework", "AudioUnit.framework", "CoreMIDI.framework", "Cocoa.framework",
            "Metal.framework", "QuartzCore.framework", "Carbon.framework", "pthread", "m",
        }
        links(macAudioFrameworks)
        -- Only the factory function leaves the bundle (src/D110EmuAU.exp).
        visibility "Hidden"
        linkoptions { "-Wl,-exported_symbols_list," .. path.getrelative(path.getabsolute(projectDir), path.getabsolute("src/D110EmuAU.exp")) }
        postbuildcommands { "sh " .. root .. "/tools/macos-app.sh plugin \"$(TARGETDIR)/../..\" " .. root .. "/src/D110EmuAU.plist" }
        warnings "Extra"
        filter "files:**.mm"
            buildoptions { "-fobjc-arc" }
        -- The SDK is C++23 (std::expected), and so is the file that includes it.
        filter "files:vendor/AudioUnitSDK/**.cpp or src/AuPlugin.mm"
            buildoptions { "-std=c++23" }
        -- Objective-C classes share one namespace in the host's process, where the VST3 build of the editor may be too
        -- (REAPER loads both): here they get names of their own.
        filter "files:src/PluginEditorMac.mm"
            buildoptions { "-DD110EmuPluginView=D110EmuAUPluginView" }
        filter "files:vendor/imgui/backends/imgui_impl_metal.mm"
            buildoptions {
                "-DMetalBuffer=D110EmuAUMetalBuffer", "-DFramebufferDescriptor=D110EmuAUFramebufferDescriptor",
                "-DMetalTexture=D110EmuAUMetalTexture", "-DMetalContext=D110EmuAUMetalContext",
            }
        filter "files:vendor/**"
            warnings "Default"
        filter {}
end

-- The emulator in a terminal: a text interface on the application's core (settings, ROMs, audio, MIDI, the memory and the
-- unit's modes, as the standalone has them), and the terminal's side.
local tuiFiles = table.join(terminalFiles, {
    "src/AppCore.h", "src/AppCore.cpp",
    "src/AudioOutput.h", "src/AudioOutput.cpp",
    "vendor/miniaudio/miniaudio.h", "vendor/miniaudio/miniaudio.c",
    "src/Lcd.h", "src/LcdSchemes.cpp",
    "src/TuiApp.h", "src/TuiApp.cpp",
})

-- The translation box in a terminal: its core with a text interface.
local translatorTuiFiles = table.join(terminalFiles, translatorFiles, translatorAppFiles, {
    "src/TranslatorTui.h", "src/TranslatorTui.cpp",
})

-- The emulator in a terminal (d110emu-tui; D110EmuTUI.exe on Windows), for headless machines and systems without a good
-- graphics backend, or without an interface (--headless) as a service. On Linux, MIDI comes from the ALSA sequencer
-- (libasound.so.2, loaded when it runs, so building needs no ALSA development package); on macOS from CoreMIDI.
project "D110EmuTUI"
    location(projectDir)
    kind "ConsoleApp"
    files(engineFiles)
    files(tuiFiles)
    files { "src/MainTui.cpp", "src/TuiMain.h" }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    defines(miniaudioDefines)
    links { "mt32emu" }
    debugdir "."
    warnings "Extra"
    filter "files:vendor/**"
        warnings "Default"
    filter "system:windows"
        files { "src/TerminalWin32.cpp", "src/MidiInputWin32.cpp" }
        files { "res/D110Emu.rc", "res/D110Emu.ico" }  -- The emulator's icon, as D110Emu.exe's
        resincludedirs { "res" }
        links { "winmm", "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:linux"
        targetname "d110emu-tui"
        files { "src/TerminalPosix.cpp", "src/MidiAlsa.h", "src/MidiInputAlsa.cpp" }
        links { "pthread", "dl", "m" }
    filter "system:macosx"
        targetname "d110emu-tui"
        files { "src/TerminalPosix.cpp", "src/MidiCoreMidi.h", "src/MidiInputCoreMidi.cpp" }
        links(macAudioFrameworks)
        links { "CoreMIDI.framework", "pthread", "m" }
    filter {}

-- The translation box in a terminal (mt32translator-tui; MT32TranslatorTUI.exe on Windows), for a machine between an
-- MT-32 program and a real unit, or without an interface (--headless) as a service. MIDI in and out: WinMM, the ALSA
-- sequencer (libasound.so.2, loaded when it runs), CoreMIDI.
project "MT32TranslatorTUI"
    location(projectDir)
    kind "ConsoleApp"
    files(translatorTuiFiles)
    files { "src/MainTranslatorTui.cpp", "src/TuiMain.h" }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    links { "mt32emu" }
    debugdir "."
    warnings "Extra"
    filter "system:windows"
        files { "src/TerminalWin32.cpp", "src/MidiInputWin32.cpp", "src/MidiOutputWin32.cpp" }
        links { "winmm", "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:linux"
        targetname "mt32translator-tui"
        files { "src/TerminalPosix.cpp" }
        files(linuxMidiFiles)
        links { "pthread", "dl", "m" }
    filter "system:macosx"
        targetname "mt32translator-tui"
        files { "src/TerminalPosix.cpp" }
        files(macMidiFiles)
        links { "CoreMIDI.framework", "CoreFoundation.framework", "pthread", "m" }
    filter {}

-- MT-32 presets against their D-110 stand-ins, rendered (needs MT-32 and D-110 ROMs): pitch, centroid, attack.
project "presetpitch"
    location(projectDir)
    kind "ConsoleApp"
    files(engineFiles)
    files { "tools/presetpitch.cpp" }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    links { "mt32emu" }
    warnings "Extra"
    filter "system:windows"
        links { "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:not windows"
        links { "pthread" }
    filter {}

-- Command-line MT-32 file translation for real units (SysEx for their memory, MIDI files).
project "mt32translate"
    location(projectDir)
    kind "ConsoleApp"
    files(translatorFiles)
    files { "tools/mt32translate.cpp" }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    links { "mt32emu" }
    warnings "Extra"
    filter "system:windows"
        links { "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:not windows"
        links { "pthread" }
    filter {}

-- Command-line renderer: MIDI file (or test pattern) to WAV through the emulation.
project "d110render"
    location(projectDir)
    kind "ConsoleApp"
    files(engineFiles)
    files { "tools/d110render.cpp" }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    links { "mt32emu" }
    warnings "Extra"
    filter "system:windows"
        links { "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:not windows"
        links { "pthread" }
    filter {}

-- Behaviour tests for the D-110 extensions (needs the ROMs; prints PASS/FAIL, exit code 0 on success).
project "d110tests"
    location(projectDir)
    kind "ConsoleApp"
    files(engineFiles)
    files { "tools/d110tests.cpp", "src/MidiPipe.h", "src/MidiPipe.cpp", "src/MidiOutput.h" }
    files { "src/PatternCapture.h", "src/PatternCapture.cpp" }
    files(toneEditorFiles)
    files { "src/ToneEditorApp.h", "src/ToneEditorApp.cpp", "src/UnitLink.h", "src/UnitLink.cpp",
            "src/UnitSetup.h", "src/UnitSetup.cpp", "src/PresetNames.h", "src/PresetNames.cpp",
            "src/Settings.h", "src/Settings.cpp", "src/UiStyle.h", "src/UiStyle.cpp" }
    -- The audio output, for the 7.1 surround test (miniaudio's null device), the terminal interface on the application's
    -- core, and MT32Translator's terminal version.
    files(tuiFiles)
    files(translatorTuiFiles)
    includedirs { "src" }
    externalincludedirs(externalDirs)
    defines(miniaudioDefines)
    links { "mt32emu", "imgui" }
    warnings "Extra"
    filter "files:vendor/**"
        warnings "Default"
    filter "system:windows"
        files { "src/MidiInputWin32.cpp", "src/MidiOutputWin32.cpp" }
        links { "winmm", "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:not windows"
        files { "src/MidiInputNull.cpp", "src/MidiOutputNull.cpp" }
        links { "pthread", "dl", "m" }
    filter "system:macosx"
        links(macAudioFrameworks)
    filter {}

-- The plugins driven as a host drives them (VST2's not on macOS, which has only the VST3 plugin).
if not targetsMac then
    -- The VST2 plugin driven as a host drives it, without a DAW and without its window (needs the ROMs; PASS/FAIL).
    project "vsttest"
        location(projectDir)
        kind "ConsoleApp"
        files(engineFiles)
        files(appFiles)
        files {
            "tools/vsttest.cpp", "tools/TestDataFolder.h", "src/Vst2.h", "src/VstPlugin.h", "src/VstPlugin.cpp",
            "src/PluginCore.h", "src/PluginCore.cpp", "src/PluginEditorNull.cpp",
            "src/MidiInputNull.cpp",
        }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "imgui" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter "system:windows"
            links { "winmm", "ole32", "comdlg32", "shell32", "uuid" }
        filter "system:not windows"
            links { "pthread", "dl", "m" }
        filter {}
end

-- The VST3 plugin driven as a host drives it, without a DAW and without its window (needs the ROMs; PASS/FAIL);
-- --module tests a built plugin (on macOS its bundle, loaded as a CFBundle).
project "vst3test"
    location(projectDir)
    kind "ConsoleApp"
    files(engineFiles)
    files(appFiles)
    files(vst3SdkFiles)
    files {
        "tools/vst3test.cpp", "tools/TestDataFolder.h", "src/Vst3Plugin.cpp", "src/PluginCore.h", "src/PluginCore.cpp",
        "src/PluginEditorNull.cpp",
        "src/MidiInputNull.cpp", "src/RolandSysex.h",
    }
    includedirs { "src" }
    externalincludedirs(externalDirs)
    externalincludedirs { "vendor/vst3sdk" }
    defines(miniaudioDefines)
    links { "mt32emu", "imgui" }
    warnings "Extra"
    filter "files:vendor/**"
        warnings "Default"
    filter "system:windows"
        links { "winmm", "ole32", "comdlg32", "shell32", "uuid" }
    filter "system:not windows"
        links { "pthread", "dl", "m" }
    filter "system:macosx"
        links(macAudioFrameworks)  -- CoreFoundation too, for the bundle
    filter {}

if not targetsWindows then
    -- The terminal interface without a terminal: TuiApp drawn into a text screen, keys typed at given times, the screen
    -- printed as text (and the terminal's escape sequences to a file); no audio or MIDI devices.
    project "tuisnap"
        location(projectDir)
        kind "ConsoleApp"
        files(engineFiles)
        files(tuiFiles)
        files(translatorTuiFiles)
        files { "tools/tuisnap.cpp", "src/MidiInputNull.cpp", "src/MidiOutputNull.cpp" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "pthread", "dl", "m" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter "system:macosx"
            links(macAudioFrameworks)
        filter {}

    -- A minimal VST3 host for Linux: the built plugin's editor in X11 windows of its own, run by a host's event loop,
    -- with input and PNG snapshots (needs an X display; Xvfb will do).
    if os.target() == "linux" then
        project "vst3host"
            location(projectDir)
            kind "ConsoleApp"
            files { "tools/vst3host.cpp", "tools/TestDataFolder.h" }
            files(vst3SdkFiles)
            externalincludedirs { "vendor/vst3sdk" }
            links { "X11", "dl", "pthread" }
            warnings "Extra"
            filter "files:vendor/**"
                warnings "Default"
            filter {}
    end

    -- Headless UI snapshots (PNG) for checking the interface on machines without a display.
    project "uisnap"
        location(projectDir)
        kind "ConsoleApp"
        files(engineFiles)
        files(appFiles)
        files { "tools/uisnap.cpp", "src/MidiInputNull.cpp", "src/MidiOutputNull.cpp" }
        files(translatorFiles)
        files(translatorAppFiles)
        files { "src/TranslatorApp.h", "src/TranslatorApp.cpp" }
        files { "src/ToneEditorApp.h", "src/ToneEditorApp.cpp", "src/UnitLink.h", "src/UnitLink.cpp",
                "src/UnitSetup.h", "src/UnitSetup.cpp" }
        files { "src/PatternCaptureApp.h", "src/PatternCaptureApp.cpp", "src/PatternCapture.h", "src/PatternCapture.cpp" }
        includedirs { "src" }
        externalincludedirs(externalDirs)
        defines(miniaudioDefines)
        links { "mt32emu", "imgui", "pthread", "dl", "m" }
        warnings "Extra"
        filter "files:vendor/**"
            warnings "Default"
        filter "system:macosx"
            links(macAudioFrameworks)
        filter {}
end

# Unit tests.
#
# Each test is a standalone executable with its own main(), matching the style
# the files were already written in. They link pitchnet_core, which exposes the
# JUCE modules PUBLIC, so a test gets every include path it needs by linking
# that one target.
#
# Built only when PITCHNET_BUILD_TESTS=ON, so a normal build and CI are
# unaffected:
#
#   cmake -B build -DPITCHNET_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

set(PITCHNET_TEST_SOURCES
    AraRegionMergeTests.cpp
    AraRegionSliceTests.cpp
    F0RepairTests.cpp
    InstanceRegistryTests.cpp
    FormantShiftTests.cpp
    HostSyncTempoTests.cpp
    NoteClipboardTests.cpp
    NoteDeleteTests.cpp
    NoteMacrosTests.cpp
    DoubleTimingTests.cpp
    NoteNameParseTests.cpp
    NoteGainCurveTests.cpp
    NonAraLifecycleTests.cpp
    OriginalWaveformRestoreTests.cpp
    PitchDrawingTests.cpp
    PitchDriftTests.cpp
    PluginStateArchiveTests.cpp
    ProjectModificationRangeTests.cpp
    ReplaceRangesTests.cpp
    SynthesisTimingTests.cpp
    TimelineCoordinateTests.cpp
    TuningTests.cpp
    VisualWaveformEnvelopeTests.cpp)

foreach(test_source IN LISTS PITCHNET_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE pitchnet_core)
    target_compile_features(${test_name} PRIVATE cxx_std_17)

    # The tests assert on results rather than returning a failure code, so a
    # failed assertion aborts the process and CTest sees a non-zero exit.
    add_test(NAME ${test_name} COMMAND ${test_name})

    # NOTE: assert() compiles to nothing under NDEBUG, which release
    # configurations set - so a test written with bare assert() passes
    # vacuously there. Undefining NDEBUG for the target is not the fix: it
    # changes the compile flags for every JUCE source the target pulls in,
    # forcing a full rebuild of JUCE per test. Tests should use a check macro
    # that does not depend on NDEBUG instead; see TestAssert.h.
endforeach()

# The capture state machine belongs to the plug-in rather than pitchnet_core.
target_sources(NonAraLifecycleTests PRIVATE
    ../Source/Plugin/NonAraCaptureController.cpp)

add_executable(PitchMacroScopeTests PitchMacroScopeTests.cpp)
target_link_libraries(PitchMacroScopeTests PRIVATE pitchnet_core)
target_compile_features(PitchMacroScopeTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroScopeTests COMMAND PitchMacroScopeTests)

# Offline analysis -> resynthesis probe for timing checks; not a CTest test,
# it needs the models and an input WAV. See tools/ResynthProbe.cpp.
add_executable(ResynthProbe ../tools/ResynthProbe.cpp)
target_link_libraries(ResynthProbe PRIVATE pitchnet_core)
target_compile_features(ResynthProbe PRIVATE cxx_std_17)

add_executable(TrackUndoTests TrackUndoTests.cpp)
target_link_libraries(TrackUndoTests PRIVATE pitchnet_core)
target_compile_features(TrackUndoTests PRIVATE cxx_std_17)
add_test(NAME TrackUndoTests COMMAND TrackUndoTests)

add_executable(TrackGestureTests TrackGestureTests.cpp)
target_link_libraries(TrackGestureTests PRIVATE pitchnet_ui)
target_compile_features(TrackGestureTests PRIVATE cxx_std_17)
add_test(NAME TrackGestureTests COMMAND TrackGestureTests)

add_executable(PitchMacroRenderTests PitchMacroRenderTests.cpp)
target_link_libraries(PitchMacroRenderTests PRIVATE pitchnet_ui)
target_compile_features(PitchMacroRenderTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroRenderTests COMMAND PitchMacroRenderTests)

# Loads a built VST3 like a host without ARA and screenshots its editor.
# Not a CTest test: it needs a display and an installed plug-in.
add_executable(PluginHostProbe ../tools/PluginHostProbe.cpp)
target_link_libraries(PluginHostProbe PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)
target_compile_definitions(PluginHostProbe PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_compile_features(PluginHostProbe PRIVATE cxx_std_17)

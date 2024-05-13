apitrace
========

This is a set of tools in the spirit of [apitrace](https://github.com/apitrace/apitrace) which can trace Direct3D, OpenGL or Vulkan calls to a file and later replay those calls from a file. The implementation is done on top of the ReShade API to make it independent from the underlying graphics APIs and consists of a ReShade add-on that traces calls to a file and an application which uses ReShade as a library that replays calls from a file.

## Building

You'll need Visual Studio 2017 or higher to build apitrace.

- To capture a trace, install ReShade to the target application and place the built add-on (`api_trace.addon32/addon64`) next to it. Then simply run the application and a trace file will be generated.
- To run the playback application, place a copy of ReShade (`ReShade64.dll`) next to the built executable (in `.\bin\x64`) and then execute it with the path to the trace file as the command-line argument.

## License

ReShade and this project are licensed under the terms of the [BSD 3-clause license](LICENSE.md).

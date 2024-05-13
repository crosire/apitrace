apitrace
========

This is a set of tools in the spirit of [apitrace](https://github.com/apitrace/apitrace) which can trace Direct3D, OpenGL or Vulkan calls to a file and later replay those calls from a file. The implementation is done on top of the ReShade API to make it independent from the underlying graphics APIs and consists of a ReShade add-on that traces calls to a file and an application which uses ReShade as a library that replays calls from a file.

## License

ReShade and this project are licensed under the terms of the [BSD 3-clause license](LICENSE.md).

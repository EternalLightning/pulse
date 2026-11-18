# Additional Fluent System Icons

`home_24_color.svg` is the unmodified Microsoft Fluent System Icons
[Home 24 color SVG](https://github.com/microsoft/fluentui-system-icons/blob/main/assets/Home/SVG/ic_fluent_home_24_color.svg),
retrieved 2026-10-01. Copyright Microsoft Corporation; MIT license included in `LICENSE`.

Files currently uses its PNG home artwork via
[SidebarSectionIcons.cs](https://github.com/files-community/Files/blob/main/src/Files.App/Helpers/UI/SidebarSectionIcons.cs).
Pulse uses the official Fluent SVG counterpart to retain scalable vector rendering.

The toolbar uses Book Open, Arrow Undo, Link, Square and Table Simple from
[Fluent System Icons at 4e8620a](https://github.com/microsoft/fluentui-system-icons/tree/4e8620a7c6ba5fba3f3f3f6908dd06b3a9332be6/assets).
The upstream paths are retained. The monochrome fill is inherited from the SVG root
so Pulse can apply its theme colors. Each file identifies its upstream SVG.

# SBARINFO Mugshot Scaling Tutorial

BiasedDoom now supports extended mugshot definitions in `SBARINFO` that allow you to specify the target width and height of the mugshot. This enables using high-resolution face graphics that are automatically scaled down to fit the status bar.

## Syntax

The `mugshot` command has been extended with two optional parameters at the end:

```c
mugshot <FaceName>, <Accuracy>, <Flags>, <X>, <Y> [, <Width>, <Height>];
```

- **FaceName**: The name of the mugshot state (e.g., "STFST").
- **Accuracy**: The update accuracy (0 for default).
- **Flags**: Flags like `health`, `directional`, etc.
- **X, Y**: The position on the status bar.
- **Width** (Optional): The target width to draw the mugshot.
- **Height** (Optional): The target height to draw the mugshot.

## Example

To display a standard Doom face ("STFST") at position (143, 168) but forced to a size of **24x29** pixels:

```c
mugshot "STFST", 0, "health", 143, 168, 24, 29;
```

If your source graphics for "STFST" are high-resolution (e.g., 96x116), the engine will automatically scale them down to 24x29 at runtime.

## Notes

- If `Width` and `Height` are omitted, the mugshot will be drawn at its original texture size (default behavior).
- Set `Width` or `Height` to `0` to use the original texture dimension for that axis.
- This feature works with both standard and custom HUDs defined in `SBARINFO`.

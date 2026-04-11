## Design Goals

 - Launch pad.  Make its easy to get something goaling quickly.
 - Indpendence.  Export/import is first class citizen.  The idea is to take
   what you build and use it in your own engine or tool.
 - Immediate mode.  The joy of immediate mode is the localized focus.  The
   geometry is in the code and not hidden behind a data file.  The user can see
   the geometry and color in the code and change it without having to open a
   separate tool.
 - Limited state.  Animation driven by time.  Particles drived by deterministic
   random number generator.
 - No textures, just geometry and color.  Not a hard design goal but the
   current idea is to expose the expressiveness of geometry and color to the
   user and not hide it behind textures.

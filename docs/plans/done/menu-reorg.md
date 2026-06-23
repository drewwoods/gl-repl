# Menu Layout Updates

I've implemented named separators and grouped similar items within the immediate-mode REPL menus exactly as specified. 

> [!NOTE]
> All unit tests have been run (using `make test-stubs`) and confirmed passing after the refactor.

### 1. `Scene` Menu Implementation
I restructured the menu bar to use `"File", "Scene", "Config"`. The new **Scene** menu now serves as the centralized hub for both Examples and Scene actions:

- **EXAMPLES**
  - Various example scenes (Lit cube, Animated ring, etc.)
- **SCENE**
  - `New empty scene`
  - `Save to output.c`

### 2. Built-in Separator UI Support
The UI panel rendering logic in `ui_panels.c` has been enhanced to understand two semantic styles natively:
- Any item name starting with `### ` is rendered as a **Header** (all-caps gray text without hover backgrounds).
- Any item name containing exactly `---` is rendered as a horizontal **Divider Line**.
- Both of these item types bypass hit-testing logic, meaning they act functionally as inert dividers.

### 3. `Config` Groups
I added structural separators directly into the source-of-truth configuration toggles (`g_cfg_items` in `repl_editor.c`). The layout is now grouped logically making it easier to parse via categories such as:
* **RENDERING**
* **TIME & REPLAY**
* **OVERLAYS & SCENE**
* **GEOMETRY**
* **INTERFACE**
* **AUDIO**

All file serialization operations (`@cfg` saving in `repl_export.c`) and cyclic hotkey mappings safety-bypass these new `NULL` separator entries to avoid conflicts or segmentation faults.


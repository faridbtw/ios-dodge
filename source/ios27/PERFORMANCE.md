# iOS 27 graphics bring-up on OnePlus 13

The first UI boot used CoreAnimation software rendering. The lower-resolution comparison was inconclusive, so the original 1125×2436 rendering size is restored.

The experimental graphics stack uses Dada XNU’s native Adreno 830 driver and the existing adapted Mesa/Turnip and NativeMetal sources. Turnip, the C/Objective-C plug-in and probes are rebuilt for arm64e/iOS 27.2. The Rust bridge and its standard library use Dada’s pauthtest target and pointer-authenticated calls and returns. The older iOS 16 arm64 binaries are not selected for this profile.

## Verified on the phone

The separate `performance-gpu-v4` test passed Vulkan instance/device creation, buffer fill/update, image clear, compute, drawing and a 170-stream submission split. Metal passed AIR compute, triangle rendering, IOSurface import, padded-row guards and mirror-buffer checks. Both reported zero failures. Evidence: `performance-gpu-v4/RESULT.json`; full logs remain local.

## Interface experiment

`performance-metal-ui-v1/PROFILE.json` enables native Metal discovery before launchd, installs the matched bundle in RAM and allows backboardd, SpringBoard and Preferences to use it. It uses the three CoreAnimation compatibility switches from Dada’s iOS boot script. GPU starts at 160 MHz; CPU frequency writes remain disabled, with a read-only SCMI probe requested. Standalone tests are removed from the UI session.

The user confirmed correct interface appearance in the initial Metal trial but still reported severe lag. A successful standalone shader test does not establish a 60/120 FPS interface.

All phone boots use `fastboot boot`; system images are uploaded into RAM. No phone partitions are written. Serial requested for this build: `OP13DDGEBTW`. The working iOS 16 defaults are preserved.

## CPU governor verification

The first Metal UI was active but the user reported that scrolling remained slow. The CPU probe measured five cores near 1.99 GHz, and firmware reported the prime cluster at its 1.017 GHz minimum. The capped-governor test measured five cores at 2.389–2.394 GHz and confirmed the prime cluster switched to its 2.2464 GHz level. Activity counters covered all seven iOS cores. CPU requests are capped at 2.4 GHz and GPU requests at 342 MHz; the governor throttles on hot or missing readings. Sampled temperatures were 46.3–54.4°C. Evidence: `performance-governor-v1/RESULT.json`. The subsequent user comparison rejected this governor trial, as described below.

The user subsequently reported approximately 1 FPS across the interface in the governor trial, with graphics visually correct. The governor profile was rejected and removed from the selected boot. Correct CPU clock scaling did not establish a responsiveness improvement.

## Whole-system stall investigation

Process accounting found heavy startup icon generation, which later settled. Thread snapshots showed CoreAnimation waiting in `IOMFBServer::set_next_update` and `register_libedr_callbacks`, with another thread waiting for libEDR IPC. The logs contain repeated EDR IPC timeouts. SpringBoard also repeatedly requested a missing stock InputUI job. The `performance-services-v1` candidate starts the stock EDR service inside backboardd through its inspected no-argument export and loads the stock InputUI launch job through launchd. Its conditional keyboard-management endpoint is omitted; the other original service definitions remain. This candidate preserves the original non-governor kernel and renderer. The user confirmed a large responsiveness improvement; native EDR startup took 7894 µs, and the captured logs had no EDR timeouts or missing InputUI label lookups. Some startup frame windows measured 35–39 FPS; these were not a controlled scrolling benchmark. Flashing black bands during animations remain unresolved.

## Transient black-band investigation

The user supplied `IMG_0698.MOV`, showing a wide missing band with stepped edges during the Settings opening animation. USB framebuffer captures reproduce the same corruption, establishing that it is already in the composed image rather than solely a panel scanout artifact.

`performance-mirror-fence-v1` waited for previous GPU buffer readers before refreshing mirrored CPU data and serialized refresh through submission. Metal correctness tests still passed, with 875 reader waits and no failures at the second timing snapshot; the user reported that black patches remained and the speed improvement remained. This is a negative result, not a confirmed rendering fix.

`performance-heap-isolation-v1` is the next diagnostic candidate. It gives automatic-heap textures real standalone allocations to isolate heap alias reuse; placement heaps keep their explicit offsets. It returns to the original fast renderer's mirror behavior and preserves native EDR, InputUI and its requested hosted keyboard endpoint. This changes heap accounting and memory usage, so it is a diagnostic candidate, not a final implementation. Metal passed with zero failures and hundreds of isolated allocations succeeded, but the user still saw the black band. This diagnostic did not fix the corruption.

`performance-cache-flush-v1` restores normal heaps and mirror behavior and adds Mesa `flushall,syncdraw` to the already used `sysmem,nocb,startup` flags. Mesa source confirms that these request all cache clean/invalidate bits and draw idle/memory waits. This is an intentionally conservative cache-ordering diagnostic, pending device and animation comparison.

## Wallpaper display and gallery

The user deferred transient animation bands and requested wallpaper repair. `wallpaper-service-v1` loads the original PosterBoard launch job with explicit startup. The job was previously absent; native prewarm callbacks alone did not create a working service. The repaired service enumerates 22 providers and creates its initial poster, but the screen remained black.

`wallpaper-metal-v1` also adds PosterBoard, MercuryPosterExtension and CollectionsPoster to the driver's explicit Metal client list. Mercury now opens the Adreno 830 device and produces nonblack IOSurface pixels. A framebuffer capture shows the stock wallpaper behind home-screen icons, and the user confirmed it is visible. Normal Mesa flags, the native EDR/InputUI speed fixes and the original non-governor kernel remain selected.

Wallpaper selection is not fixed yet. Opening the gallery caused `JETSAM_REASON_MEMORY_VMCOMPRESSOR_SPACE_SHORTAGE` kills, including CollectionsPoster and backboardd, resulting in an automatic respring. The current boot argument caps compressed memory at 16,384 pages. `wallpaper-memory-v1` tests 65,536 pages with the same kernel and root image; this reserves more compressor capacity without enabling storage or swap. This increase needs boot and gallery validation because a much larger compressor reservation caused a KVA claim failure during early bring-up.

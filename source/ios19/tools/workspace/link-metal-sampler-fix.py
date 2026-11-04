import os
from pathlib import Path
import subprocess,hashlib,json
b=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve();project=b.parents[1]/'OnePlus13-iOS-RAM';g=project/'build/ios27-gpu';v=project/'experimental/ios27/v2';bundle=b/'boot-root/lib/RedwoodMetal.bundle';lib=bundle/'RedwoodMetal';bridge=b/'metal-ios-target/arm64e-apple-ios-pauth/release/libsolarium_native_metal_bridge.a';assert bridge.is_file()
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
cmd=['xcrun','clang++','-target','arm64e-apple-ios27.2','-isysroot',sdk,'-O2','-fstack-protector-strong','-bundle',str(g/'plugin/NativeAdrenoMetalPlugin.o'),str(v/'wallpaper-ui-v1/NativeBridgeMetalFrontend.o'),str(g/'plugin/AdrenoPM4.o'),str(bridge),'-Wl,-dead_strip','-lobjc','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreFoundation','-o',str(lib)]
subprocess.run(cmd,check=True);subprocess.run(['xcrun','strip','-x',str(lib)],check=True)
temp=lib.with_name('RedwoodMetal.versioned');subprocess.run(['xcrun','vtool','-set-build-version','ios','18.0','19.0','-replace','-output',str(temp),str(lib)],check=True);temp.replace(lib);lib.chmod(0o755)
subprocess.run(['codesign','-f','-s','-',str(bundle)],check=True);subprocess.run(['codesign','--verify','--deep','--strict',str(bundle)],check=True)
(b/'READ-SAMPLER-FIX.json').write_text(json.dumps({'status':'host captured-shader compile passed; device wake test pending','legacy_intrinsic':'air.get_read_sampler_texture_2d','mapping':'existing zero-argument GetReadSampler operation; same addrspace2 sampler ABI','shader':'brim_jump_lph from original native19 QuartzCore, kept local','renderer_sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),'kernel_unchanged':True,'isolated_source':'metal2vulkan-source','preserved_visible_build':'verified-lockscreen-v1'},indent=2)+'\n')
print('Relinked isolated renderer with legacy read-sampler support.',flush=True)

import os
from pathlib import Path
import os,subprocess,hashlib
b=Path(os.environ['IOS19_RAM_DIR']).expanduser().resolve();g=b.parents[1]/'OnePlus13-iOS-RAM/build/ios27-gpu';project=b.parents[1]/'OnePlus13-iOS-RAM'
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip();cc=subprocess.check_output(['xcrun','--sdk','iphoneos','--find','clang'],text=True).strip();env=os.environ.copy();rid=hashlib.sha256((b/'metal2vulkan-source/crates/metal2vulkan-air/src/intrinsics/texture.rs').read_bytes()).hexdigest()[:32]
env.update(PATH=str(g/'nightly-toolchain/bin')+':'+env['PATH'],RUSTC=str(g/'nightly-toolchain/bin/rustc'),CARGO_HOME=str(project/'build/dada-gpu/cargo'),CARGO_NET_OFFLINE='true',CARGO_BUILD_JOBS='2',NMB_RUNTIME_ID=rid,NMB_FIXTURE_AIR=str(b/'metal-bridge/runtime_examples.air'),NMB_FC_AIR=str(b/'metal-bridge/function_constant.air'),IPHONEOS_DEPLOYMENT_TARGET='27.2',PAUTHTEST_SYSROOT=sdk,CC_arm64e_apple_ios_pauth=cc,CFLAGS_arm64e_apple_ios_pauth='-target arm64e-apple-ios27.2 -isysroot '+sdk,RUSTFLAGS='-Dwarnings -Zpointer-authentication=+calls,+return-addresses,+auth-traps,+intrinsics')
subprocess.run([str(g/'nightly-toolchain/bin/cargo'),'build','--release','--manifest-path',str(b/'metal-bridge/Cargo.toml'),'--target',str(b/'arm64e-apple-ios-pauth.json'),'-Zjson-target-spec','-Zbuild-std=std,panic_unwind','--target-dir',str(b/'metal-ios-target')],env=env,check=True)
print('Built isolated iOS renderer bridge; runtime ID',rid,flush=True)

import os
import subprocess
import shutil
from pathlib import Path

APP_PATH = Path("build/exported/CelestronOriginMonitor.app")
EXECUTABLE = APP_PATH / "Contents/MacOS/CelestronOriginMonitor"
FRAMEWORKS_DIR = APP_PATH / "Contents/Frameworks"
SIGN_IDENTITY = "Apple Development: Mr Jonathan Kimmitt (5AU5B5HJQX)"

def run(cmd):
    print(f"> {cmd}")
    result = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        print(result.stdout)
    return result.stdout

def list_links(binary):
    out = run(f"otool -L {binary}")
    lines = out.strip().split('\n')[1:]
    return [line.strip().split(' ')[0] for line in lines]

def fix_install_name(binary, old, new):
    run(f"install_name_tool -change {old} {new} {binary}")

def set_id(lib_path, new_id):
    run(f"install_name_tool -id {new_id} {lib_path}")

def copy_dependency(src_path):
    dst_path = FRAMEWORKS_DIR / os.path.basename(src_path)
    if not dst_path.exists():
        shutil.copy(src_path, dst_path)
        print(f"Copied {src_path} -> {dst_path}")
    return dst_path

def fix_binary(binary):
    links = list_links(binary)
    for lib in links:
        if lib.startswith("/opt/homebrew"):
            local_copy = copy_dependency(lib)
            new_path = f"@executable_path/../Frameworks/{local_copy.name}"
            fix_install_name(binary, lib, new_path)

def fix_all():
    FRAMEWORKS_DIR.mkdir(parents=True, exist_ok=True)

    # Fix main binary
    fix_binary(EXECUTABLE)

    # Fix embedded frameworks
    for fw in FRAMEWORKS_DIR.glob("*"):
        if fw.is_file() and fw.suffix == ".dylib":
            set_id(fw, f"@rpath/{fw.name}")
            fix_binary(fw)
        elif fw.is_dir() and fw.name.endswith(".framework"):
            fw_binary = fw / "Versions/A/" / fw.name.replace(".framework", "")
            if fw_binary.exists():
                set_id(fw_binary, f"@rpath/{fw.name}/Versions/A/{fw_binary.name}")
                fix_binary(fw_binary)

    # Resign app
    run(f'codesign --deep --force --options runtime --sign "{SIGN_IDENTITY}" "{APP_PATH}"')

if __name__ == "__main__":
    fix_all()
    

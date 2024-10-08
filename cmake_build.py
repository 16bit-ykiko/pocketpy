import os
import sys
import shutil

assert os.system("python prebuild.py") == 0

if not os.path.exists("build"):
    os.mkdir("build")

assert len(sys.argv) <= 2

if len(sys.argv) == 2:
    config = sys.argv[1]
else:
    config = 'Release'

assert config in ['Debug', 'Release', 'RelWithDebInfo']

os.chdir("build")

code = os.system(f"cmake .. -DPK_ENABLE_OS=ON -DCMAKE_BUILD_TYPE={config}")
assert code == 0
code = os.system(f"cmake --build . --config {config}")
assert code == 0

if sys.platform == "win32":
    shutil.copy(f"{config}/main.exe", "../main.exe")
    shutil.copy(f"{config}/pocketpy.dll", "../pocketpy.dll")
elif sys.platform == "darwin":
    shutil.copy("main", "../main")
    shutil.copy("libpocketpy.dylib", "../libpocketpy.dylib")
else:
    shutil.copy("main", "../main")
    shutil.copy("libpocketpy.so", "../libpocketpy.so")

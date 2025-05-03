1. Create a new terminal and run the compilation command:

Compile command (Windows vcpkg): 
cl /EHsc OP.c
/I"%VCPKG_ROOT%\installed\x64-windows\include"
/link /LIBPATH:"%VCPKG_ROOT%\installed\x64-windows\lib"
cjson.lib
libcurl.lib
ws2_32.lib
crypt32.lib

Compile command (macOS Homebrew): 
gcc OP_Old.c -o OP_Old
-I/opt/homebrew/include
-I/opt/homebrew/include/cjson
-L/opt/homebrew/lib
-lcjson -lcurl -lpthread

2. Create a new terminal

Run the local server and view the map: python3 -m http.server 8000

Browser access http://localhost:8000/map.html

Note: OP.c and map.html must be placed in the same directory.

1.创建一个新的终端并运行编译命令：

编译命令（Windows vcpkg）：cl /EHsc OP.c
/I"%VCPKG_ROOT%\installed\x64-windows\include"
/link /LIBPATH:"%VCPKG_ROOT%\installed\x64-windows\lib"
cjson.lib
libcurl.lib
ws2_32.lib
crypt32.lib

编译命令（macOS Homebrew）：
gcc OP_Old.c -o OP_Old
-I/opt/homebrew/include
-I/opt/homebrew/include/cjson
-L/opt/homebrew/lib
-lcjson -lcurl -lpthread

2.创建新终端

运行本地服务器并查看地图：python3-m http.server 8000

浏览器访问
http://localhost:8000/map.html

注意：OP.c和map.html必须放在同一个目录中。

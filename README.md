* 编译命令（Windows vcpkg）：
    cl /EHsc OP.c \
        /I"%VCPKG_ROOT%\installed\x64-windows\include" \
        /link /LIBPATH:"%VCPKG_ROOT%\installed\x64-windows\lib" \
            cjson.lib \
            libcurl.lib \
            ws2_32.lib \
            crypt32.lib
 * 
 * 编译命令（macOS Homebrew）：
    gcc OP_Old.c -o OP_Old \
      -I/opt/homebrew/include \
      -I/opt/homebrew/include/cjson \
      -L/opt/homebrew/lib \
      -lcjson -lcurl -lpthread
 *
 * 运行本地服务器并查看地图：
 *   python3 -m http.server 8000
 *   浏览器访问 http://localhost:8000/map.html

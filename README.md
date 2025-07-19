- nginx config is in /usr/local/nginx/conf/nginx.conf
- nginx executable is in /usr/local/nginx/sbin

# Julia language extension for Nginx

Use the julia language from inside nginx config. You can place code snippets that can also `include()` julia files. You can receive HTTP header and body data from requests and also send HTTP responses from julia.

Goals:
- julia header filter
- julia body filter
- content_by_julia directive
- julia proxy directive

This is inpired by and very similar to OpenResty which implements a Lua language extension to nginx among other things.

### How to build:

1. Download nginx source code from [github](https://github.com/nginx/nginx) or from the nginx website [download page](https://nginx.org/en/download.html). I am using nginx-1.21.3.
  ```
  wget https://nginx.org/download/nginx-1.21.3.tar.gz
  ```

2. Extract the downloaded nginx source code archive. Clone this repo into the `modules` directory.
  ```
  tar -xzf nginx-1.21.3.tar.gz
  cd nginx-1.21.3/modules
  git clone https://github.com/nkottary/ngx-http-julia-module
  ```

3. Open the `config` file and modify the `JULIA_DIR` variable to point to the julia binary directory on your computer. For example, I use julia 1.11.2, my julia binary directory is `~/.julia/juliaup/julia-1.11.2+0.x64.linux.gnu`.

4. Configure nginx build environment. Run `configure` with the `--add-module` option:

```
./configure --add-module=modules/ngx_http_julia_module
```

5. Build the nginx binary. We will build our module into nginx. There are ways to build the Julia module separately and link the binary to nginx, but that is yet to be explored. Navigate to the nginx-1.21.3 directory and run:
  ```
  configure
  make
  ```

### References:
The following guides helped me learn about nginx module development:
- [tejgop](https://github.com/tejgop)'s [nginx module guide](https://tejgop.github.io/nginx-module-guide)
- [Emiller's Guide To Nginx Module Development](https://www.evanmiller.org/nginx-modules-guide.html)

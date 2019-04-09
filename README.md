nginx post handler
=====
[post_handler](https://github.com/aiwhj/post_handler.git) 的 Nginx 版，嵌入了 PHP-embed，能对 HTTP 请求实现拦截，通过 Header 中是否有相应的 key，判断是否允许当次请求。
做这个的原因是想对 HTTP 大文件上传拦截。php-fpm 默认会直接上传文件，在传递 body 之前，能先对 header 进行判断。

扩展一下可以做成 Nginx 拓展版的 SAPI，类似 Apache 的 apache2handler。

## install
1. 需要 PHP Embed SAPI 的共享库，可以实现通过 `--enable-embed` 编译出来共享库。
2. 下载源码
```shell
git clone https://github.com/aiwhj/ngx_post_handler.git
```
3. 修改 config 编译配置
```shell
ngx_addon_name=ngx_http_post_handler_module
HTTP_MODULES="$HTTP_MODULES ngx_http_post_handler_module"
NGX_ADDON_SRCS="$NGX_ADDON_SRCS $ngx_addon_dir/ngx_http_post_handler_module.c"
CORE_INCS="$CORE_INCS /Users/roger/.phpbrew/php/php-7.1.15-embed/include/php/ \
            /Users/roger/.phpbrew/php/php-7.1.15-embed/include/php/main \
            /Users/roger/.phpbrew/php/php-7.1.15-embed/include/php/Zend \
            /Users/roger/.phpbrew/php/php-7.1.15-embed/include/php/TSRM \
            -Wall -g"
CORE_LIBS="$CORE_LIBS -lphp7"

```
4. 把拓扑编译进 Nginx
```shell
cd nginx
./configure --add-module=/source/ngx_post_handler
make && make install
```
5. location 下配置关键字 `post_handler`，值为想要运行的 PHP 脚本。
```conf
location ~ \.php$ {
    try_files $uri =404;

    post_handler "if (!isset($_SERVER['HTTP_HANDLER']) || $_SERVER['HTTP_HANDLER'] != 'post') { echo 'do not receive the body' . PHP_EOL; exit(1); }";

    include fastcgi.conf;
    fastcgi_pass php71;
}
```
也可以直接使用 php 输出。
```conf
location /hello_world {
    post_handler "echo 'this is a php script'; exit(1);";
}
```
## 注意
`exit status` 必须返回一个大于 0 的整数
当 `exit status` 是一个大于 0 的整数的时候，PHP 脚本能输出。

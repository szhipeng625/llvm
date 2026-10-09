path = 'runtime/server.cpp'
src = open(path, encoding='utf-8').read()

old = '''void handleRequest(SSL *ssl, int clientFd) {
  char buffer[8192];
  ssize_t n = tlsRecv(ssl, buffer, sizeof(buffer) - 1);
  if (n <= 0) { close(clientFd); return; }
  buffer[n] = '\\0';
  std::string request(buffer, static_cast<size_t>(n));'''

new = '''void handleRequest(SSL *ssl, int clientFd) {
  char buffer[8192];
  std::string request;
  // 循环读取直到收齐完整 HTTP 请求:头结束 + Content-Length 指定的请求体。
  // 客户端常把请求头与请求体分多次发送,单次读取会拿到不完整的请求。
  while (request.size() < sizeof(buffer) - 1) {
    size_t room = sizeof(buffer) - 1 - request.size();
    ssize_t n = tlsRecv(ssl, buffer, static_cast<int>(room));
    if (n <= 0) break;
    request.append(buffer, static_cast<size_t>(n));
    size_t hdrEnd = request.find("\\r\\n\\r\\n");
    if (hdrEnd == std::string::npos) continue;  // 头还没收完
    size_t clPos = request.find("Content-Length:");
    if (clPos != std::string::npos && clPos < hdrEnd) {
      size_t vStart = clPos + 15;
      while (vStart < hdrEnd && (request[vStart] == ' ' || request[vStart] == '\\t')) vStart++;
      size_t vEnd = request.find("\\r\\n", vStart);
      long cl = std::atol(request.substr(vStart, vEnd - vStart).c_str());
      if (request.size() >= hdrEnd + 4 + static_cast<size_t>(cl)) break;
    } else {
      break;  // 无请求体(GET 等),头收完即完整
    }
  }
  if (request.empty()) { close(clientFd); return; }'''

assert old in src, 'handleRequest anchor not found'
src = src.replace(old, new, 1)
open(path, 'w', encoding='utf-8').write(src)
print('full request read applied')

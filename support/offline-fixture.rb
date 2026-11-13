# frozen_string_literal: true
require "socket"
server = TCPServer.new("0.0.0.0", 8080)
fixture = File.binread(File.join(__dir__, "fixture.html"))
worker = Thread.new do
  loop do
    socket = server.accept
    begin
      request = socket.gets.to_s
      while (line = socket.gets) && line != "\r\n"; end
      body = request.split[1] == "/child" ? '<input id="known" type="file" multiple>' : fixture
      socket.write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n")
      socket.write(body)
    ensure
      socket.close
    end
  end
end
begin
  load ARGV.fetch(0)
ensure
  worker.kill
  server.close
  worker.join
end

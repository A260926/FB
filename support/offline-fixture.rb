# frozen_string_literal: true
require "socket"
server = TCPServer.new("0.0.0.0", 8080)

child_page = <<~HTML
  <!doctype html><meta charset="utf-8"><title>Child frame</title>
  <input id="known" type="file" multiple>
  <button id="child-attach">Child attach</button>
  <iframe src="/grandchild"></iframe>
  <script>
  window.childEvents = [];
  window.childAttachments = [];
  document.querySelector('#child-attach').onclick = () => {
    const input = document.createElement('input');
    input.type = 'file'; input.hidden = true; input.multiple = true;
    input.addEventListener('input', e => window.childEvents.push({type: e.type, trusted: e.isTrusted}));
    input.addEventListener('change', e => {
      window.childEvents.push({type: e.type, trusted: e.isTrusted});
      window.childAttachments = Promise.all(Array.from(input.files, async f => ({name: f.name, text: await f.text()})));
    });
    document.body.append(input);
    input.click();
  };
  </script>
HTML

grandchild_page = <<~HTML
  <!doctype html><meta charset="utf-8"><title>Grandchild frame</title>
  <input id="known" type="file" multiple>
  <button id="grand-attach">Grandchild attach</button>
  <script>
  window.grandEvents = [];
  window.grandAttachments = [];
  document.querySelector('#grand-attach').onclick = () => {
    const input = document.createElement('input');
    input.type = 'file'; input.hidden = true; input.multiple = true;
    input.addEventListener('input', e => window.grandEvents.push({type: e.type, trusted: e.isTrusted}));
    input.addEventListener('change', e => {
      window.grandEvents.push({type: e.type, trusted: e.isTrusted});
      window.grandAttachments = Promise.all(Array.from(input.files, async f => ({name: f.name, text: await f.text()})));
    });
    document.body.append(input);
    input.click();
  };
  </script>
HTML

sibling_page = <<~HTML
  <!doctype html><meta charset="utf-8"><title>Sibling frame</title>
  <input id="known" type="file" multiple>
HTML

fixture = File.binread(File.join(__dir__, "fixture.html"))
worker = Thread.new do
  loop do
    socket = server.accept
    begin
      request = socket.gets.to_s
      while (line = socket.gets) && line != "\r\n"; end
      path = request.split[1].to_s
      body = case path
             when "/child" then child_page
             when "/grandchild" then grandchild_page
             when "/sibling" then sibling_page
             when "/" then fixture
             else "<!doctype html><meta charset=\"utf-8\"><title>#{path}</title><p>#{path}</p>"
             end
      headers = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n" \
                "Content-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n"
      socket.write(headers)
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

# frozen_string_literal: true
require "json"
require "ferrum"

browser = Ferrum::Browser.new(ws_url: ENV.fetch("FERRUM_WS_URL"), timeout: 10)
begin
  page = browser.create_page
  page.go_to(ENV.fetch("FERRUM_FIXTURE_URL"))
  file = File.expand_path("upload.txt", __dir__)
  page.at_css("#known").select_file(file)
  result = page.evaluate("await document.querySelector('#known').files[0].text()")
  raise "file contents differ" unless result == File.read(file)
  events = page.evaluate("window.uploadEvents")
  raise "native events differ: #{events}" unless events == ["input", "change"]
  puts "KNOWN_NODE_NATIVE_UPLOAD_OK bytes=#{result.bytesize} events=#{events.join(',')}"

  nested = page.frames.find { |frame| frame != page.main_frame }
  raise "child frame missing" unless nested
  nested.at_css("#known").select_file(file)
  result = nested.evaluate("await document.querySelector('#known').files[0].text()")
  raise "iframe upload differs" unless result == File.read(file)
  puts "FRAME_NODE_NATIVE_UPLOAD_OK"
  puts(page.respond_to?(:capture_file_chooser) ? "CAPTURE_FILE_CHOOSER_API_PRESENT" : "CAPTURE_FILE_CHOOSER_API_ABSENT")
ensure
  browser.quit
end

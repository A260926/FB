# frozen_string_literal: true
require "json"
require "tmpdir"
require "fileutils"
require "ferrum"

browser = Ferrum::Browser.new(ws_url: ENV.fetch("FERRUM_WS_URL"), timeout: 10)
base_url = ENV.fetch("FERRUM_FIXTURE_URL")

def assert(condition, message)
  raise "contract assertion failed: #{message}" unless condition
end

def wait_until(deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 5)
  last_error = nil
  loop do
    result = begin
      yield
    rescue Ferrum::Error => e
      last_error = e
      nil
    end
    return result if result

    if Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
      raise last_error || "wait_until timed out"
    end

    sleep 0.05
  end
end

def wait_for_frames(page, count)
  wait_until { page.frames.size >= count && page.frames.all?(&:idle?) }
  page.frames
end

tmpdir = Dir.mktmpdir("chooser-contract", __dir__)
invoice = File.join(tmpdir, "invoice.txt")
receipt = File.join(tmpdir, "receipt.txt")
note = File.join(tmpdir, "note.txt")
missing = File.join(tmpdir, "missing.txt")
File.write(invoice, "invoice contents\n")
File.write(receipt, "receipt contents\n")
File.write(note, "note contents\n")

begin
  page = browser.create_page
  page.go_to(base_url)
  wait_for_frames(page, 4) # main, child, grandchild, sibling

  # ---------------------------------------------------------------- CHOOSER_CAPTURE_OK
  block_ran = false
  begin
    page.capture_file_chooser
    raise "expected ArgumentError without a block"
  rescue ArgumentError
  end
  assert(!block_ran, "capture without a block must raise before running anything")

  begin
    page.capture_file_chooser(frame: Object.new) { raise "must not run" }
    raise "expected ArgumentError for a foreign frame"
  rescue ArgumentError
  end

  detached = Ferrum::Frame.new("dead-frame", page)
  begin
    page.capture_file_chooser(frame: detached) { raise "must not run" }
    raise "expected ArgumentError for a removed frame"
  rescue ArgumentError
  end

  [0, -1, Float::INFINITY, Float::NAN, "10"].each do |bad_timeout|
    begin
      page.capture_file_chooser(timeout: bad_timeout) { raise "must not run" }
      raise "expected ArgumentError for timeout #{bad_timeout.inspect}"
    rescue ArgumentError
    end
  end

  user_events = 0
  user_listener = page.on("Page.fileChooserOpened") { user_events += 1 }

  chooser = page.capture_file_chooser { page.at_css("#attach").click }
  assert(chooser.is_a?(Ferrum::FileChooser), "capture returns a Ferrum::FileChooser")
  assert(chooser.frame_id == page.main_frame.id, "chooser carries the frame id")
  assert(chooser.backend_node_id.is_a?(Integer) && chooser.backend_node_id.positive?,
         "chooser carries a backend node id")
  assert(chooser.multiple? == true, "the multiple input yields a multiple chooser")
  read_only = !chooser.respond_to?(:frame_id=) &&
              !chooser.respond_to?(:backend_node_id=) &&
              !chooser.respond_to?(:"multiple?=")
  assert(read_only, "chooser metadata is read-only")
  assert(user_events >= 1, "caller-registered event listeners keep firing")
  assert(page.subscribed?("Page.fileChooserOpened"), "the caller listener stays subscribed after capture")

  returned = chooser.accept([invoice, receipt])
  assert(returned.equal?(chooser), "accept returns the chooser itself")
  wait_until { page.evaluate("window.attachments.length") == 2 }
  names = page.evaluate("window.attachments.map(a => a.name)")
  assert(names == [File.basename(invoice), File.basename(receipt)], "files keep input order, got #{names.inspect}")
  attachments = page.evaluate("window.attachments")
  assert(attachments == [{ "name" => File.basename(invoice), "text" => File.read(invoice) },
                         { "name" => File.basename(receipt), "text" => File.read(receipt) }],
         "the browser reads real file names and contents")
  assert(page.evaluate("window.lastInput.files.length") == 2, "the DOM input holds both files")
  events = page.evaluate("window.chooserEvents")
  assert(events == [{ "type" => "input", "trusted" => true }, { "type" => "change", "trusted" => true }],
         "the browser fires trusted input/change, got #{events.inspect}")
  attributes = page.evaluate(<<~JS)
    ({
      multiple: window.lastInput.multiple,
      accept: window.lastInput.accept,
      form: window.lastInput.form && window.lastInput.form.name
    })
  JS
  assert(attributes == { "multiple" => true, "accept" => ".txt", "form" => nil }, "input attributes are untouched")

  begin
    chooser.accept(note)
    raise "expected Ferrum::Error on a second accept"
  rescue Ferrum::Error
  end
  assert(page.evaluate("window.chooserEvents.length") == 2, "a rejected second accept fires no second event round")

  reentered = false
  page.capture_file_chooser do
    begin
      page.capture_file_chooser { reentered = true }
      raise "expected Ferrum::Error for a reentrant capture"
    rescue Ferrum::Error
    end
    assert(!reentered, "the inner block must never run")
    page.at_css("#attach").click
  end

  runs = 0
  begin
    page.capture_file_chooser(timeout: 0.5) { runs += 1 }
    raise "expected Ferrum::TimeoutError without a chooser"
  rescue Ferrum::TimeoutError
  end
  assert(runs == 1, "the block runs exactly once")

  begin
    page.capture_file_chooser { raise RuntimeError, "boom" }
    raise "expected the block exception to surface"
  rescue RuntimeError => e
    assert(e.message == "boom", "the block exception is re-raised as-is")
  end

  next_chooser = page.capture_file_chooser { page.at_css("#attach").click }
  next_chooser.accept(invoice)
  wait_until { page.evaluate("window.attachments.length") == 1 }

  other_page = browser.create_page
  other_page.go_to(base_url)
  wait_for_frames(other_page, 4)
  other_chooser = other_page.capture_file_chooser { other_page.at_css("#attach").click }
  other_chooser.accept(receipt)
  wait_until { other_page.evaluate("window.attachments.length") == 1 }
  assert(other_page.evaluate("window.attachments[0].text") == File.read(receipt), "another page captures independently")
  other_page.close
  page.off("Page.fileChooserOpened", user_listener)

  puts "CHOOSER_CAPTURE_OK"

  # ---------------------------------------------------------------- CHOOSER_UPLOAD_OK
  single = page.capture_file_chooser { page.at_css("#single-attach").click }
  assert(single.multiple? == false, "the plain input yields a single-file chooser")

  begin
    single.accept([invoice, receipt])
    raise "expected ArgumentError uploading two files to a single chooser"
  rescue ArgumentError
  end
  assert(page.evaluate("window.lastInput.files.length") == 0, "single-mode rejection uploads nothing")

  begin
    single.accept([])
    raise "expected ArgumentError for an empty array"
  rescue ArgumentError
  end

  begin
    single.accept(missing)
    raise "expected ArgumentError for a missing file"
  rescue ArgumentError
  end
  assert(page.evaluate("window.lastInput.files.length") == 0, "missing-file rejection uploads nothing")

  begin
    single.accept(tmpdir)
    raise "expected ArgumentError for a directory"
  rescue ArgumentError
  end

  assert(single.accept(invoice).equal?(single), "a corrected accept works after validation failures")
  wait_until { page.evaluate("window.attachments.length") == 1 }
  assert(page.evaluate("window.lastInput.files[0].name") == File.basename(invoice), "single upload lands on the input")
  begin
    single.accept(receipt)
    raise "expected Ferrum::Error reusing a consumed chooser"
  rescue Ferrum::Error
  end

  multi = page.capture_file_chooser { page.at_css("#attach").click }
  begin
    multi.accept([invoice, missing, receipt])
    raise "expected ArgumentError for a bad path inside a multi upload"
  rescue ArgumentError
  end
  assert(page.evaluate("window.lastInput.files.length") == 0, "atomic validation never uploads the valid prefix")
  assert(multi.accept([receipt, invoice]).equal?(multi), "the chooser stays usable after a failed validation")
  wait_until { page.evaluate("window.attachments.length") == 2 }
  names = page.evaluate("window.attachments.map(a => a.name)")
  assert(names == [File.basename(receipt), File.basename(invoice)], "order is preserved, got #{names.inspect}")
  texts = page.evaluate("window.attachments.map(a => a.text)")
  assert(texts == [File.read(receipt), File.read(invoice)], "multi upload contents come from the browser")

  relative = page.capture_file_chooser { page.at_css("#attach").click }
  relative.accept(File.join("support", "upload.txt"))
  wait_until { page.evaluate("window.attachments.length") == 1 }
  assert(page.evaluate("window.attachments[0].name") == "upload.txt", "relative paths are expanded")

  puts "CHOOSER_UPLOAD_OK"

  # ---------------------------------------------------------------- CHOOSER_FRAME_OK
  child = page.frames.reject(&:main?).find { |f| f.url.end_with?("/child") }
  assert(child, "the child frame is present")
  frame_chooser = page.capture_file_chooser(frame: child) { child.at_css("#child-attach").click }
  assert(frame_chooser.frame_id == child.id, "the chooser is bound to the child frame id")
  frame_chooser.accept(invoice)
  wait_until { child.evaluate("Promise.resolve(window.childAttachments).then(a => a.length)") == 1 }
  child_names = child.evaluate("Promise.resolve(window.childAttachments).then(a => a.map(x => x.name))")
  assert(child_names == [File.basename(invoice)], "child frame upload reads the file in the frame")
  assert(child.evaluate("window.childEvents") == [{ "type" => "input", "trusted" => true },
                                                  { "type" => "change", "trusted" => true }],
         "child frame events are native")

  grandchild = page.frames.reject(&:main?).find { |f| f.url.end_with?("/grandchild") }
  assert(grandchild, "the nested grandchild frame is present")
  grand_chooser = page.capture_file_chooser(frame: grandchild) { grandchild.at_css("#grand-attach").click }
  assert(grand_chooser.frame_id == grandchild.id, "nested same-origin iframes are supported")
  grand_chooser.accept(receipt)
  wait_until { grandchild.evaluate("Promise.resolve(window.grandAttachments).then(a => a.length)") == 1 }
  grand_text = grandchild.evaluate("Promise.resolve(window.grandAttachments).then(a => a[0].text)")
  assert(grand_text == File.read(receipt), "grandchild upload contents are real")

  begin
    page.capture_file_chooser(frame: child, timeout: 0.5) { page.at_css("#attach").click }
    raise "expected TimeoutError when the chooser opens in another frame"
  rescue Ferrum::TimeoutError
  end
  recovered = page.capture_file_chooser(frame: child) { child.at_css("#child-attach").click }
  recovered.accept(receipt)
  wait_until { child.evaluate("Promise.resolve(window.childAttachments).then(a => a.length)") == 1 }

  puts "CHOOSER_FRAME_OK"

  # ---------------------------------------------------------------- CHOOSER_STALE_OK
  stale_input = page.capture_file_chooser { page.at_css("#attach").click }
  page.evaluate("window.lastInput.remove()")
  begin
    stale_input.accept(invoice)
    raise "expected NodeNotFoundError after the input was removed"
  rescue Ferrum::NodeNotFoundError
  end

  replaced = page.capture_file_chooser { page.at_css("#attach").click }
  page.evaluate(<<~JS)
    const old = window.lastInput;
    const clone = old.cloneNode();
    old.replaceWith(clone);
    window.lastInput = clone;
  JS
  begin
    replaced.accept(invoice)
    raise "expected NodeNotFoundError after the input was replaced"
  rescue Ferrum::NodeNotFoundError
  end
  assert(page.evaluate("window.lastInput.files.length") == 0, "the replacement node never receives the upload")

  detached_frame_chooser = page.capture_file_chooser(frame: child) { child.at_css("#child-attach").click }
  page.evaluate("document.querySelector('iframe[src=\"/child\"]').remove()")
  wait_until { page.frames.reject(&:main?).none? { |f| f.id == child.id } }
  begin
    detached_frame_chooser.accept(invoice)
    raise "expected NodeNotFoundError after the frame was detached"
  rescue Ferrum::NodeNotFoundError
  end

  navigated = page.capture_file_chooser { page.at_css("#attach").click }
  page.go_to(base_url)
  wait_for_frames(page, 4)
  begin
    navigated.accept(invoice)
    raise "expected NodeNotFoundError after a cross-document navigation"
  rescue Ferrum::NodeNotFoundError
  end

  hashed = page.capture_file_chooser { page.at_css("#attach").click }
  page.evaluate("location.hash = 'bookkeeping'")
  hashed.accept(invoice)
  wait_until { page.evaluate("window.attachments.length") == 1 }
  hash_text = page.evaluate("window.attachments[0].text")
  assert(hash_text == File.read(invoice), "hash-only navigation keeps the chooser valid")

  sibling = page.frames.reject(&:main?).find { |f| f.url.end_with?("/sibling") }
  assert(sibling, "the sibling frame is present")
  child = page.frames.reject(&:main?).find { |f| f.url.end_with?("/child") }
  sibling_safe = page.capture_file_chooser(frame: child) { child.at_css("#child-attach").click }
  sibling.evaluate("location.href = '/elsewhere'")
  wait_until { sibling.url.end_with?("/elsewhere") }
  sibling_safe.accept(receipt)
  wait_until { child.evaluate("Promise.resolve(window.childAttachments).then(a => a.length)") == 1 }

  final_chooser = page.capture_file_chooser { page.at_css("#attach").click }
  final_chooser.accept([invoice, receipt])
  wait_until { page.evaluate("window.attachments.length") == 2 }
  final_names = page.evaluate("window.attachments.map(a => a.name)")
  assert(final_names == [File.basename(invoice), File.basename(receipt)],
         "the next capture after every failure path still works")

  puts "CHOOSER_STALE_OK"
ensure
  FileUtils.remove_entry(tmpdir) if tmpdir && File.directory?(tmpdir)
  browser.quit
end

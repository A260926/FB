# frozen_string_literal: true

module Ferrum
  #
  # A native file chooser opened inside a {Page}, captured with
  # {Page#capture_file_chooser} instead of a file input selector. It keeps the
  # `backendNodeId` of the input the browser opened the chooser for, the frame
  # and document identity captured at that moment, and whether the chooser
  # accepts one file or several. Files are handed to the browser with
  # {#accept}, which goes through the same `DOM.setFileInputFiles` call as
  # {Node#select_file}.
  #
  class FileChooser
    # The id of the frame the chooser's file input lives in.
    #
    # @return [String]
    attr_reader :frame_id

    # The backend node id of the file input, taken verbatim from the
    # `Page.fileChooserOpened` event.
    #
    # @return [Integer]
    attr_reader :backend_node_id

    #
    # @param [Page] page
    #   The page the chooser was captured on.
    #
    # @param [Frame] frame
    #   The frame the `Page.fileChooserOpened` event came from.
    #
    # @param [Hash{String => Object}] params
    #   The raw `Page.fileChooserOpened` event params.
    #
    # @param [String, nil] loader_id
    #   The frame's loader id when the event arrived, its document identity.
    #
    def initialize(page, frame, params, loader_id:)
      @page = page
      @frame_id = frame.id
      @backend_node_id = params["backendNodeId"]
      @multiple = params["mode"] == "selectMultiple"
      @loader_id = loader_id
      @used = false
      @mutex = Mutex.new
    end

    # Whether the chooser accepts several files, i.e. whether the input has
    # the `multiple` attribute.
    #
    # @return [Boolean]
    def multiple?
      @multiple
    end

    #
    # Hands files to the captured input via `DOM.setFileInputFiles`, the same
    # call {Node#select_file} uses. Every path is validated before anything
    # is sent to the browser, so a rejected argument leaves the input
    # untouched and the chooser reusable.
    #
    # @param [String, Array<String>] paths
    #   One path, or a non-empty array of paths. Relative paths are expanded
    #   against the Ruby process's working directory and input order is
    #   preserved; each path must point at a readable regular file.
    #
    # @return [self]
    #
    # @raise [ArgumentError]
    #   If the list is empty, a path is not a readable regular file, or a
    #   single-file chooser gets several files. Nothing reaches the browser.
    #
    # @raise [Ferrum::NodeNotFoundError]
    #   If the captured input was removed/replaced, its frame was detached or
    #   its document navigated since the capture.
    #
    # @raise [Ferrum::Error]
    #   If the chooser has already been accepted once.
    #
    # @example
    #   chooser = page.capture_file_chooser { page.at_css("#attach").click }
    #   chooser.accept(["invoice.pdf", "receipt.pdf"])
    #
    def accept(paths)
      @mutex.synchronize do
        raise Error, "file chooser has already been used" if @used

        files = validate_paths(paths)
        ensure_still_current!
        @page.command("DOM.setFileInputFiles",
                      slowmoable: true,
                      backendNodeId: @backend_node_id,
                      files: files)
        @used = true
      end

      self
    end

    private

    def validate_paths(paths)
      files = Array(paths)
      raise ArgumentError, "supply at least one file path" if files.empty?
      unless files.all? { |path| path.is_a?(String) && !path.empty? }
        raise ArgumentError, "file paths must be non-empty strings"
      end
      if !@multiple && files.size > 1
        raise ArgumentError, "this file chooser accepts a single file only"
      end

      files.map do |path|
        absolute = File.expand_path(path)
        unless File.file?(absolute) && File.readable?(absolute)
          raise ArgumentError, "#{path.inspect} is not a readable regular file"
        end

        absolute
      end
    end

    def ensure_still_current!
      # A detached frame takes its document with it. A same-document change
      # (e.g. only the URL hash) keeps both the frame and the captured node.
      frame = @page.frame_by(id: @frame_id)
      raise stale_node_error unless frame

      # The backend node id outlives the frontend id but dies with its
      # document: resolving it is the authoritative check and covers a removed
      # or replaced input and a cross-document navigation, while a hash-only
      # change keeps the same document and still resolves.
      id = @page.command("DOM.pushNodesByBackendIdsToFrontend",
                         backendNodeIds: [@backend_node_id]).dig("nodeIds", 0)
      raise stale_node_error if id.nil? || id.zero?
    rescue NodeNotFoundError
      raise stale_node_error
    end

    def stale_node_error
      @stale_node_error ||= NodeNotFoundError.new(
        {
          "message" => "the captured file input is gone: it was removed or replaced, " \
                       "its frame was detached or its document navigated"
        }
      )
    end
  end
end

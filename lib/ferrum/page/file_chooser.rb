# frozen_string_literal: true

require "ferrum/file_chooser"

module Ferrum
  class Page
    #
    # Captures the native file chooser the block opens, e.g. by clicking a
    # button that creates a hidden file input on click, when no stable
    # selector exists for that input. Interception of the browser's file
    # chooser and the `Page.fileChooserOpened` listener are both installed
    # before the block runs and torn down afterwards, whatever the outcome.
    #
    module FileChooserCapture
      #
      # Runs the block and captures the next native file chooser opened in
      # the given frame. Only the chooser of that exact frame is accepted;
      # choosers opened in the top frame or sibling frames are ignored.
      #
      # @param [Frame] frame
      #   The frame the chooser is expected in, defaults to the main frame.
      #
      # @param [Numeric] timeout
      #   Monotonic deadline in seconds, measured from the moment
      #   interception is ready until the matching event arrives. Defaults
      #   to the page's timeout. The block itself is never interrupted.
      #
      # @yield The code that opens the file chooser, run exactly once.
      #
      # @return [Ferrum::FileChooser]
      #
      # @raise [ArgumentError]
      #   If no block is given, `frame` belongs to another page or has been
      #   removed, or `timeout` isn't a positive finite number. Raised before
      #   the block runs.
      #
      # @raise [Ferrum::Error]
      #   If another capture is already running on the same page.
      #
      # @raise [Ferrum::TimeoutError]
      #   If no matching chooser opens before the deadline.
      #
      # @example
      #   chooser = page.capture_file_chooser { page.at_css("#attach").click }
      #   chooser.accept([invoice_path, receipt_path])
      #
      def capture_file_chooser(frame: main_frame, timeout: self.timeout)
        raise ArgumentError, "a block is required to open the file chooser" unless block_given?
        unless frame.is_a?(Frame) && frame.page.equal?(self) && frame_by(id: frame.id).equal?(frame)
          raise ArgumentError, "frame belongs to another page or has been removed"
        end
        unless timeout.is_a?(Numeric) && timeout.finite? && timeout.positive?
          raise ArgumentError, "timeout must be a positive finite number, got #{timeout.inspect}"
        end

        unless @file_chooser_mutex.try_lock
          raise Error, "a file chooser capture is already running on this page"
        end

        subscription = nil
        begin
          command("Page.setInterceptFileChooserDialog", enabled: true)
          deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + timeout
          gate = Concurrent::IVar.new

          # The listener outlives the block on purpose: the click's CDP
          # response can return before the subscriber thread dispatches the
          # matching event, so it stays installed until the wait settles.
          subscription = on("Page.fileChooserOpened") do |params|
            next unless params["frameId"] == frame.id

            gate.try_set([params,
                          frame.loader_id,
                          Process.clock_gettime(Process::CLOCK_MONOTONIC)])
          end

          begin
            yield
          rescue StandardError
            cleanup_file_chooser_capture(subscription)
            subscription = nil
            raise
          end

          remaining = deadline - Process.clock_gettime(Process::CLOCK_MONOTONIC)
          gate.wait(remaining) if remaining.positive?

          if gate.complete? && gate.value!.last <= deadline
            params, loader_id, = gate.value!
            FileChooser.new(self, frame, params, loader_id: loader_id)
          else
            raise TimeoutError
          end
        ensure
          cleanup_file_chooser_capture(subscription) if subscription
          @file_chooser_mutex.unlock
        end
      end

      private

      def cleanup_file_chooser_capture(subscription)
        off("Page.fileChooserOpened", subscription)
        command("Page.setInterceptFileChooserDialog", enabled: false)
      rescue StandardError
        # The page or its connection may already be gone; the capture's
        # listeners die with the client either way.
      end
    end
  end
end

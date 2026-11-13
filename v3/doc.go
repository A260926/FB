/*
Package grab provides a HTTP download manager implementation.

Get is the most simple way to download a file:

	resp, err := grab.Get("/tmp", "http://example.com/example.zip")
	// ...

Get will download the given URL and save it to the given destination directory.
The destination filename will be determined automatically by grab using
Content-Disposition headers returned by the remote server, or by inspecting the
requested URL path.

An empty destination string or "." means the transfer will be stored in the
current working directory.

If a destination file already exists, grab will assume it is a complete or
partially complete download of the requested file. If the remote server supports
resuming interrupted downloads, grab will resume downloading from the end of the
partial file. If the server does not support resumed downloads, the file will be
retransferred in its entirety. If the file is already complete, grab will return
successfully.

For control over the HTTP client, destination path, auto-resume, checksum
validation and other settings, create a Client:

	client := grab.NewClient()
	client.HTTPClient.Transport.DisableCompression = true

	req, err := grab.NewRequest("/tmp", "http://example.com/example.zip")
	// ...
	req.NoResume = true
	req.HTTPRequest.Header.Set("Authorization", "Basic YWxhZGRpbjpvcGVuc2VzYW1l")

	resp := client.Do(req)
	// ...

You can monitor the progress of downloads while they are transferring:

	client := grab.NewClient()
	req, err := grab.NewRequest("", "http://example.com/example.zip")
	// ...
	resp := client.Do(req)

	t := time.NewTicker(time.Second)
	defer t.Stop()

	for {
		select {
		case <-t.C:
			fmt.Printf("%.02f%% complete\n", resp.Progress())

		case <-resp.Done:
			if err := resp.Err(); err != nil {
				// ...
			}

			// ...
			return
		}
	}

# Reconnecting interrupted downloads

By default a connection that drops while the response body is being read ends
the transfer and leaves a partially written file. Set Request.MaxReconnects to
have grab continue the same download inside one Client.Do call instead:

	req.MaxReconnects = 3

Reconnects apply only to bodyless GET requests transferred with the identity
content encoding, and only when the initial response carries a known total
length and a strong ETag. When the body is interrupted, grab sends
"Range: bytes=k-" together with "If-Range" from the final URL that served the
body, where k is the number of bytes already written, including any local
prefix resumed at startup. A 206 response is validated byte-for-byte against
the current representation before it is appended; a 200 response signals a
changed representation and restarts the download from zero after re-running
BeforeCopy. Writer, rate limiter, hook and context errors are never retried,
and once the allowance is exhausted the written prefix is kept and the last
read error is returned. A zero MaxReconnects preserves the original behavior;
a negative value fails before any request is sent.
*/
package grab

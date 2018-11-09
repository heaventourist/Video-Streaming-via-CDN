# Video Streaming via CDN
* Implement adaptive bitrate selection, DNS load balancing, and an HTTP proxy server to stream video at high bit rates from the closest server to a given client.
* The HTTP proxy accepts connections from web browsers, modifies video chunk requests, resolves the web server’s DNS name, opens a connection with the resulting IP address, forwards the modified request to the server and returns the unmodified video chunks to the browser.
* The DNS server implements load balancing in two different ways: round-robin and geographic distance.

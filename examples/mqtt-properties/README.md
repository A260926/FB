# MQTT5 property iteration

See detailed tutorials at https://mongoose.ws/tutorials/mqtt/

This example parses a complete MQTT5 PUBLISH packet held in a local byte
buffer (no network connection is made) and walks its property section with
`mg_mqtt_prop_next()`. It prints each decoded property, stops cleanly at the
end of the section, and demonstrates that a failed decode leaves the caller's
property and offset untouched.

The decoded `key`/`val` views are borrowed from the packet buffer and stay
valid only for as long as that buffer does; copy them if you need to keep them.

## nanopi-bluetooth

Enables bluetooth on NanoPi M3, on the onboard AMPAK ap6212 wifi/bt
device.


### about ap6212hciattach

It is hciattach program stripped down to support only bcm43xx protocol,
with one timy change: delay after firmware load is increased from 2ms to
100ms. It seems the ap6212 chip needs at least 60ms delay after reset.

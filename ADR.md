# Decision records

* Due to the some similarity between the `Quectel EC800M` module and the
`Lynq L511` module, we can try to develop a driver for `Lynq L511` and use some
of the components of the resulting driver on the `Quectel EC800M`.
* Development can starts off from the readily available `urban-power-4g` board
first then move on to actual product hardware. This diversify the development
platform. Some sample code can be used in other products as well.
* The design of the driver should change to allow faster boot sequence. Current
solution will wait for all the init sequence to return which can take a long
time to complete. This will also clear up some of the configuration options
such as `REQUIRE_NETWORK_AT_BOOT` which is a very clunky solution. This can be
achieve through these steps:
    * In the `init` function, only check for the validity of the `AT` commands 
    reception and no further. Other setup/init sequence can be performed
    asynchronously in a work queue thread.
    * Used the netif interface to signal to the user application that the modem
    is ready to receive packets.

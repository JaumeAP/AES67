# Shopping list

Keyed to the reference designators in `HARDWARE.md`, section "Bill of
materials". That file says why each part is what it is; this one says where to
get it and roughly what it costs.

**Prices checked September 2026, and they move.** Where a figure below is
marked *found*, it came off a shop page or a search result on that date. Where
it says *check*, no price was confirmed and the range is an estimate, not a
quotation. Nothing here has been ordered.

Shipping and IVA are not included in any figure. For a build this small they
are not a rounding error: three parcels from three countries can cost more than
the parts.

## The board

| Ref | Part | Where | Price |
|---|---|---|---|
| - | Teensy 4.1 | [PJRC](https://www.pjrc.com/store/teensy41.html), [SparkFun](https://www.sparkfun.com/teensy-4-1.html). In Spain: [BricoGeek](https://tienda.bricogeek.com/teensy/1528-teensy-41.html), [Ultra-lab](https://ultra-lab.net/producto/teensy-4-1/), [Tiendatec](https://www.tiendatec.es/maker-zone/microcontroladores/1456-teensy-41.html), [RS](https://es.rs-online.com/web/p/kits-de-desarrollo-de-microcontroladores/2836911) | *check*, expect 35 to 45 EUR |
| - | Ethernet Kit for Teensy 4.1 | [PJRC](https://www.pjrc.com/store/ethernet_kit.html), [Opencircuit](https://opencircuit.shop/product/ethernet-kit-for-teensy-4.1), [Eckstein](https://eckstein-shop.de/PJRCEthernetKitforTeensy41EN), [Antratek](https://www.antratek.com/ethernet-kit-for-teensy-4-1) | 16 EUR at Opencircuit, *found* |

Buy the version without headers soldered if there is a choice: the box wants
the board flat on standoffs, and the pins that matter here are 13, 14, 15, 19
and 24, which are easier to wire to bare pads than to a header already in the
way.

## Word clock conditioning

| Ref | Part | Where | Price |
|---|---|---|---|
| U1 | TLV3501AIDBVR, SOT-23-6 | [TI](https://www.ti.com/product/TLV3501/part-details/TLV3501AIDBVR), [DigiKey](https://www.digikey.com/en/products/detail/texas-instruments/TLV3501AIDBVR/1669420), [LCSC](https://www.lcsc.com/product-detail/Comparators_Texas-Instruments-TLV3501AIDBVR_C193413.html), [TME](https://www.tme.com/us/en-us/details/tlv3501aidbvr/smd-comparators/texas-instruments/) | *check*, expect 3 to 6 EUR in ones |
| - | SOT-23-6 to DIP breakout | [SparkFun BOB-00717](https://www.digikey.com/en/products/detail/sparkfun-electronics/00717/5318740), [SchmalzTech ST-SOT23-6](https://www.digikey.com/en/products/detail/schmalztech-llc/ST-SOT23-6/15283224) | 1.25 USD at SparkFun, *found* |
| R1 to R9, Rf | Resistors, 1% metal film or 0603 | any distributor | a few EUR the lot |
| C1 to C4 | 100 nF and 1 uF, X7R | any distributor | a few EUR the lot |
| D1 | BAT54S | any distributor | under 1 EUR |
| D2 | SS14 | any distributor | under 1 EUR |

Buy the resistors and capacitors as a handful of each value rather than one
each. The difference in cost is smaller than one more order, and step 3 of
`BRINGUP.md` may well want a different Rf.

## Panel

| Ref | Part | Where | Price |
|---|---|---|---|
| J1 | 75 ohm chassis BNC, isolated. Neutrik `NBB75FI`, or Amphenol `031-4803-75` | [Neutrik at Avacab](https://avacab-audiovisuales.com/es/conectores-bnc/1810-neutrik-nbb75fi-conector-bnc-75-ohm-hembra-de-chasis.html), [Amphenol](https://www.amphenolrf.com/en-us/part/031-4803-75/7283/) | *check*, expect 5 to 12 EUR |
| J2 | Neutrik etherCON `NE8FDP` | [Neutrik](https://www.neutrik.com/en/product/ne8fdp), [Thomann](https://www.thomann.es/neutrik_ne8fdp.htm) | 14.10 EUR for the `-TOP` at Thomann, *found*. The black `NE8FDP-B` is around 20 USD abroad |
| J3 | Barrel jack, 2.1 x 5.5 mm, panel | any distributor | 2 to 4 EUR, *check* |
| J4 | USB type B panel socket | any distributor | 4 to 8 EUR, *check* |
| LED1 | Green LED and panel holder | any distributor | under 2 EUR |

If the etherCON is dropped for a plain RJ45 feedthrough, as `HARDWARE.md`
allows for a small printed box, that line goes from around 14 EUR to around 4.

## Enclosure and assembly

| Part | Where | Price |
|---|---|---|
| Filament, PETG or PLA | wherever you print | the box is small, a few EUR of material |
| M3 heat-set inserts and screws | any distributor | 5 to 10 EUR for a pack, *check* |
| M2.5 self-tapping screws for the boards | any distributor | under 5 EUR |
| Solid core wire, cable ties | any distributor | already in the drawer, probably |

## Outside the box

| Part | Where | Price |
|---|---|---|
| 5 V adapter, 1 A or more, certified | any distributor | 8 to 15 EUR, *check* |
| RG59 75 ohm coax with BNC ends | any pro audio shop | 5 to 15 EUR depending on length |
| USB lead to type B | already in the drawer | - |

## What it comes to

Roughly **110 to 160 EUR** of parts, of which the Teensy and its kit are about
half and the etherCON is the single most expendable line. Add shipping.

That figure is worth putting next to the one in `COMPARATIVA-P4.md`, which
argues this project may not be worth continuing separately at all. Buying the
parts is the point at which that decision stops being reversible.

## What is NOT on this list

The instruments in `BRINGUP.md`: an oscilloscope, a multimeter, and for step 9
a time interval counter. The last one is the expensive item in this whole
project and it is the only way to measure the accuracy. Borrow it.

# libASPL — subconjunt vendored

Aquest directori **no** és un submòdul git. Conté només `include/` i `src/`
de [gavv/libASPL](https://github.com/gavv/libASPL), baixats directament via
l'API de GitHub, sense `test/`, `doc/`, `examples/`, `scripts/`, CI ni el
`README`/`CMakeLists.txt` propis del projecte — no calen per compilar
`AES67Driver`, i evitar-los estalvia clonar-ho tot (~779 KB → ~676 KB, i
sobretot cap historial git addicional ni fitxers que mai es compilen aquí).

- **Origen**: https://github.com/gavv/libASPL
- **Commit**: `633e0f70203edd87d320fc5a3cae901e1363aac5` (tag `v3.1.2-1-g633e0f7`)
- **Llicència**: MIT (`LICENSE`), més codi derivat d'exemples d'Apple sota
  les seves pròpies llicències (`LICENSE.apple2012`, `LICENSE.apple2020`) —
  totes tres es mantenen aquí intactes.

Per actualitzar a un commit més nou, repetir la mateixa baixada selectiva
amb el nou hash — o tornar a fer-ho servir com a submòdul complet si algun
dia cal `test/`/`examples/` per depurar-hi alguna cosa.

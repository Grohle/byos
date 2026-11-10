<p align="center"><img src="docs/media/hibyos_intro.gif" width="420" alt="HiByOS"></p>

# HiByOS (en español)

Firmware comunitario para el **HiBy R3 II (2025)**. Mantiene intactos el reproductor y el
audio de HiBy y construye a su alrededor un sistema al estilo iPod e iOS:
- inicio con páginas y dock;
- gestos configurables y multitarea;
- **Centro de control** propio;
- barra de estado rediseñada;
- Archivos, editor de etiquetas y teclado iOS;
- tienda de apps (`.hpk`);
- funciones ocultas desbloqueadas: **8 filtros del DAC**, modo coche, solo USB sin cargar y
  modos de energía.

Todo está en inglés, español y francés.

> **Proyecto dirigido por [Grohle](https://github.com/Grohle) y ejecutado en su mayor parte por
> agentes de IA.** Grohle marcó los objetivos, la dirección de diseño y el listón de calidad,
> y probó cada versión en el aparato. La ingeniería inversa, el código en C, las herramientas
> y esta documentación son obra de agentes de IA bajo esa dirección.

La documentación completa está en inglés en el [README](README.md). Los informes de
investigación de [docs/](docs) están en español.

## Instalación rápida

1. Guarda el firmware oficial `r3ii_2025.upt` (de la web de HiBy) por si quieres volver.
2. Revisa la tarjeta con `chkdsk X: /f`.
3. Copia en la **raíz de la SD** el `r3ii_2025.upt` y la carpeta `HiByOS/` del
   [release](https://github.com/Grohle/hibyos/releases/latest).
4. En el reproductor: **Ajustes › Actualización de firmware › Vía tarjeta SD**. Tarda
   alrededor de 1 minuto.
5. En el primer arranque, HiByOS instala sus ficheros desde `HiByOS/`. El registro queda en
   `HiByOS/install.log`.

**Volver al firmware oficial:** copia el `.upt` oficial a la raíz de la SD y actualiza igual.
Si la interfaz no responde, crea un fichero vacío `REINSTALAR` en la carpeta `hibymod/` de
la SD y enciende el aparato.

**Apps:** copia los `.hpk` en la carpeta `Apps/` de la SD y ábrelos desde **Apps** en el
inicio. Para cerrar una app, desliza hacia arriba desde la barra inferior.

## Licencia

Uso gratuito para cualquier fin **no comercial**. Para venderlo o distribuirlo con ánimo
de lucro hace falta una licencia comercial. Para pedirla, o cualquier otra licencia
especial, contacta con [Grohle](https://github.com/Grohle) abriendo un *issue*. Ver
[LICENSE](LICENSE).

# Guía obligatoria de UI y animaciones (para cualquier agente que trabaje en hibymod)

## Objetivo
Rehacer **toda** la interfaz del HiBy R3 II como una mezcla de **iPod clásico + iOS moderno**.
Debe comportarse casi en todo como un clon de iOS: gestos, física, jerarquía y tipografía.
La meta no es "que funcione": es que **impresione** en tres ejes a la vez.

1. **Artístico**: proporciones del prototipo (`prototipo/r3-glass.html`), cristal, sombras suaves, tipografía limpia y nada que parezca "genérico" o improvisado.
2. **Intuitivo**: gestos de iOS.
   - Abajo hacia arriba lleva a inicio o abre el reproductor.
   - El borde lateral es "atrás".
   - Pulsación larga lleva a edición o menú contextual.
   - Al arrastrar un icono contra el borde, cambia de página.
   - Nada de botones inventados si iOS usa un gesto.
3. **Rendimiento**:
   - 44 fps sostenidos, que es el máximo del panel, sin tirones ni parpadeos ni pantallas intermedias (grises, negras o de fábrica).
   - Ningún fotograma con contenido a medio dibujar.
   - Memoria acotada.

## Bucle "gauntlet" (no se cierra una tarea sin él)
Cada animación o pantalla pasa este bucle **hasta quedar impresionado**, no hasta que "parezca bien":

1. **Grabar cada fotograma real**: `hibyctl rec MS /usr/data/hibymod/rX.bin N`. Nunca en `/tmp`, que está en RAM.
2. **Revisarlo**: `tools/recsheet.py rX.bin rX.png` hace la hoja de contactos con los ms entre fotogramas. En rojo aparece lo que supera 30 ms.
3. **Buscar**:
   - fotogramas a medio dibujar o que parpadean;
   - contenido erróneo (imágenes en caché viejas, pantallas de fábrica);
   - saltos de posición;
   - huecos de más de 30 ms;
   - estados intermedios que iOS no mostraría.
4. **Medir la CPU y la memoria** del shell (`t/shellstat.sh`). Compilar siempre con coma flotante por hardware (`musleabihf`; soft-float es 14x más lento).
5. **Corregir y repetir**. Solo se da por terminada cuando la hoja de fotogramas es impecable **y** se ha probado con el dedo en el aparato.
6. **Comparar con iOS real**: curvas (ease-out cúbico o muelle), duraciones (250–350 ms), seguimiento 1:1 del dedo y velocidad al soltar.

## Reglas técnicas aprendidas (no repetir errores)
- **Panel ST77903 sin memoria propia, barrido continuo desde la DDR a 44,3 Hz** (`docs/investigacion_60fps.md`): `FBIOPAN_DISPLAY` solo elige el búfer y espera al final del fotograma. Se escribe primero y se presenta después; escribir en el búfer que se está barriendo parpadea. 44,3 fps es el techo real (el panel admite como mucho ~50 Hz): las animaciones se diseñan para 44 Hz sin saltos, no para 60.
- **Copia compartida del shell**: va con cerrojo de secuencia (`fr0_seq`, `ov_seq`). Nunca se debe copiar una imagen a medio dibujar.
- **Capas**: se actualizan sin apagarlas nunca entre medias.
- **Presentaciones parciales** (`present_rows`) cuando solo cambia una zona, como la píldora de volumen o la barra de estado.
- **Detección del inicio de fábrica**: por puntos del **fondo de pantalla**, nunca por iconos, que cambian al pulsarlos.
- **El inicio de fábrica es un espejo del launcher** (`launcher/mirror.png` con los iconos de fábrica transparentes). La librería sustituye su primer fotograma por el launcher (`home_sub`).
- **El volumen** se lee de la barra de estado con plantillas reales de HiBy (`src/digits.h`), nunca de `user.ini`, que va con retraso.
- **Memoria**: el shell usa ~4.5 MB y quedan ~7–11 MB libres. Los búferes grandes se reutilizan (`img_under`, `img_sheet`, `img_base`, `img_app`) y no se añaden nuevos sin medir.
- **Idiomas**: todo el texto nuevo en inglés, francés y español.

## Alcance pendiente
- Centro de control propio (desplegar a la primera, animación fluida).
- Cada menú de HiBy rediseñado al estilo de la casa.
- Editor de etiquetas con autocompletado y edición múltiple con progreso.
- Teclado configurable (altura, fila de números, ES/EN/FR).
- Radio desde canción, mezclas visibles, fondos en `/Pictures`, YouTube Music (relay).

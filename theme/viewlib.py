"""Lectura/escritura de los ficheros .view/.dlg/.listview de HiBy (litegui).

Son JSON con claves repetidas ("viewgroup", "textview"... varias veces en el mismo
objeto), asi que se representan como listas ordenadas de pares [clave, valor].
Al escribir se respeta el estilo original (tabuladores, sin espacios tras ':').
"""
import json


def loads(text):
    return json.loads(text, object_pairs_hook=lambda pairs: [list(p) for p in pairs])


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return loads(f.read())


def _dump(v, ind):
    t = "\t" * ind
    if isinstance(v, list) and (not v or isinstance(v[0], list) and len(v[0]) == 2 and isinstance(v[0][0], str)):
        if not v:
            return "{}"
        items = [f'{t}\t"{k}":{_dump(x, ind + 1)}' for k, x in v]
        return "{\n" + ",\n".join(items) + f"\n{t}}}"
    return json.dumps(v, ensure_ascii=False)


def dumps(obj):
    return _dump(obj, 0) + "\n"


def save(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps(obj))


# ---- navegacion -------------------------------------------------------------
def get(node, key, default=None):
    for k, v in node:
        if k == key:
            return v
    return default


MARKERS = {"viewgroup", "textview", "imageview", "numview", "progress_bar", "clockview",
           "hglview", "layout", "listview", "editview", "scrollview"}


def set_(node, key, value):
    """Cambia o anade una propiedad. REGLA DEL MOTOR (probada en el aparato): las
    propiedades deben ir ANTES del marcador de tipo ("viewgroup":true...); lo que va
    despues se aplica al elemento siguiente."""
    for p in node:
        if p[0] == key:
            p[1] = value
            return
    pos = len(node)
    for i in range(len(node) - 1, -1, -1):
        if node[i][1] is True and node[i][0] in MARKERS or (node[i][1] is True and i == len(node) - 1):
            pos = i
            break
    node.insert(pos, [key, value])


def delete(node, key):
    node[:] = [p for p in node if p[0] != key]


def children(node):
    """Elementos hijos (viewgroup/textview/imageview/... que son objetos)."""
    return [(k, v) for k, v in node if isinstance(v, list) and v and isinstance(v[0], list)]


def walk(node, kind=None):
    """Recorre todos los elementos (kind, nodo) en profundidad."""
    for k, v in children(node):
        yield k, v
        yield from walk(v, kind)


def find(root, name):
    for k, v in walk(root):
        if get(v, "name") == name:
            return v
    return None


def element(kind, **props):
    """Crea un elemento nuevo: element('viewgroup', name=..., x=..., ...)."""
    node = [[k, v] for k, v in props.items()]
    node.append([kind, True])
    return node

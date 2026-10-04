"""Safe expressions and ``{{ templates }}`` for declarative workflows.

Expressions are a small Python subset: literals, names from the namespace (``inputs``, ``steps``,
``iteration``, ``state``, ``previous``), attribute / item access on data, comparisons, ``and/or/not``,
``in``, arithmetic, and the functions ``len any all min max abs str int float round lower``. Nothing
else (no calls on objects, no imports, no dunder access).
"""

from __future__ import annotations

import ast
import json
import re
from collections.abc import Callable
from typing import Any

_FUNCS: dict[str, Callable[..., Any]] = {
    "len": len,
    "any": any,
    "all": all,
    "min": min,
    "max": max,
    "abs": abs,
    "str": str,
    "int": int,
    "float": float,
    "round": round,
    "lower": lambda s: str(s).lower(),
    "bool": bool,
}
_BINOPS: dict[type[ast.operator], Callable[[Any, Any], Any]] = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a / b,
    ast.Mod: lambda a, b: a % b,
    ast.FloorDiv: lambda a, b: a // b,
}
_CMPS: dict[type[ast.cmpop], Callable[[Any, Any], bool]] = {
    ast.Eq: lambda a, b: a == b,
    ast.NotEq: lambda a, b: a != b,
    ast.Lt: lambda a, b: a < b,
    ast.LtE: lambda a, b: a <= b,
    ast.Gt: lambda a, b: a > b,
    ast.GtE: lambda a, b: a >= b,
    ast.In: lambda a, b: a in b,
    ast.NotIn: lambda a, b: a not in b,
    ast.Is: lambda a, b: a is b,
    ast.IsNot: lambda a, b: a is not b,
}


class ExpressionError(ValueError):
    pass


def _get(obj: Any, key: Any) -> Any:
    if isinstance(obj, dict):
        return obj.get(key)
    if isinstance(obj, (list, tuple, str)) and isinstance(key, int):
        return obj[key] if -len(obj) <= key < len(obj) else None
    if isinstance(key, str) and not key.startswith("_"):
        return getattr(obj, key, None)
    return None


def evaluate(expr: str, ns: dict[str, Any]) -> Any:
    """Evaluates a restricted expression against a namespace (missing data reads as None)."""
    try:
        tree = ast.parse(expr.strip(), mode="eval")
    except SyntaxError as e:
        raise ExpressionError(f"invalid expression {expr!r}: {e.msg}") from e
    return _eval(tree.body, ns)


def _eval(node: ast.AST, ns: dict[str, Any]) -> Any:
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Name):
        if node.id in ("True", "False", "None"):
            return {"True": True, "False": False, "None": None}[node.id]
        if node.id in ns:
            return ns[node.id]
        if node.id in _FUNCS:
            return _FUNCS[node.id]
        raise ExpressionError(f"unknown name '{node.id}' (use inputs, steps, iteration, state, previous)")
    if isinstance(node, ast.Attribute):
        if node.attr.startswith("_"):
            raise ExpressionError("private attributes are not allowed")
        return _get(_eval(node.value, ns), node.attr)
    if isinstance(node, ast.Subscript):
        return _get(_eval(node.value, ns), _eval(node.slice, ns))
    if isinstance(node, ast.BoolOp):
        vals = node.values
        if isinstance(node.op, ast.And):
            out: Any = True
            for v in vals:
                out = _eval(v, ns)
                if not out:
                    return out
            return out
        out = False
        for v in vals:
            out = _eval(v, ns)
            if out:
                return out
        return out
    if isinstance(node, ast.UnaryOp):
        val = _eval(node.operand, ns)
        if isinstance(node.op, ast.Not):
            return not val
        if isinstance(node.op, ast.USub):
            return -val
        if isinstance(node.op, ast.UAdd):
            return +val
    if isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
        return _BINOPS[type(node.op)](_eval(node.left, ns), _eval(node.right, ns))
    if isinstance(node, ast.Compare):
        left = _eval(node.left, ns)
        for op, comp in zip(node.ops, node.comparators, strict=True):
            right = _eval(comp, ns)
            try:
                if not _CMPS[type(op)](left, right):
                    return False
            except TypeError:
                return False
            left = right
        return True
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in _FUNCS and not node.keywords:
        return _FUNCS[node.func.id](*[_eval(a, ns) for a in node.args])
    if isinstance(node, (ast.List, ast.Tuple)):
        return [_eval(e, ns) for e in node.elts]
    if isinstance(node, ast.Dict):
        return {_eval(k, ns): _eval(v, ns) for k, v in zip(node.keys, node.values, strict=True) if k is not None}
    if isinstance(node, ast.IfExp):
        return _eval(node.body, ns) if _eval(node.test, ns) else _eval(node.orelse, ns)
    raise ExpressionError(f"unsupported expression: {ast.dump(node)[:80]}")


_TEMPLATE = re.compile(r"\{\{\s*(.+?)\s*\}\}")


def render(template: str, ns: dict[str, Any]) -> str:
    """Replaces ``{{ expr }}`` with the evaluated value (dicts and lists as JSON, None as empty)."""

    def sub(m: re.Match[str]) -> str:
        val = evaluate(m.group(1), ns)
        if val is None:
            return ""
        if isinstance(val, (dict, list)):
            return json.dumps(val, ensure_ascii=False, default=str)
        return str(val)

    return _TEMPLATE.sub(sub, template)


def render_value(value: Any, ns: dict[str, Any]) -> Any:
    """Renders templates inside nested args (a string that is exactly ``{{ expr }}`` keeps its type)."""
    if isinstance(value, str):
        m = _TEMPLATE.fullmatch(value.strip())
        if m:
            return evaluate(m.group(1), ns)
        return render(value, ns)
    if isinstance(value, list):
        return [render_value(v, ns) for v in value]
    if isinstance(value, dict):
        return {k: render_value(v, ns) for k, v in value.items()}
    return value

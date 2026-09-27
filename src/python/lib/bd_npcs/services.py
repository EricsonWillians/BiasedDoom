"""Services and the shop economy for :mod:`bd_npcs`.

:class:`Service` is the contract NPC offers implement:
``available(npc_id, ctx) -> bool`` gates the offer and
``run(npc_id, ctx) -> {"ok": bool, "message": str}`` performs it. The
``ctx`` dict the mod passes carries ``pawn`` (the player Actor handle),
``character`` (a ``bd_dnd.Character`` or None), ``dispositions`` (the
``bd_npcs`` store), ``manager`` (the ``NPCManager``), ``shop`` (the
NPC's :class:`Shop` or None), plus arbitrary mod keys; services read
what they need and ignore the rest.

``bd_dnd`` is imported lazily inside the functions that need it, so
importing ``bd_npcs`` never drags the d20 rules in at module import
time (the same convention ``bd_dialogue`` uses for ``bd_vtm``/``bd_dnd``).

The sellable-listing caveat
---------------------------

The engine exposes no generic "list everything the player carries"
query, only per-class ``inventory_count``. A shop's sell section can
therefore only cover the explicit :attr:`Shop.sellables` class names:
the :class:`ShopUI` lists those classes with ``inventory_count > 0``
and hides everything else.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import biaseddoom as bd

from .disposition import STANDINGS

__all__ = ["Service", "HealerService", "TrainerService", "Shop", "ShopUI"]

#: Sell price (in currency units) for an item class the shop buys back
#: but does not stock: the flat 40% ratio against the classic low-tier
#: base price of 25. List the item in ``Shop.stock`` when a custom
#: price is wanted.
UNLISTED_SELL_PRICE = 10

#: Stock UI sound for a successful transaction (stock Doom UI sound).
_TRADE_SOUND = "menu/change"


def _player_pawn() -> Any:
    """Live handle to the console player's pawn, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        pawn = player.actor
        if pawn is not None and pawn.valid:
            return pawn
    except Exception:
        pass
    return None


def _pay(pawn: Any, currency_class: str, amount: int) -> bool:
    """Take ``amount`` currency units from ``pawn``; False when short."""
    try:
        amount = int(amount)
    except (TypeError, ValueError):
        return False
    if amount <= 0:
        return True
    try:
        if int(pawn.inventory_count(currency_class)) < amount:
            return False
        pawn.take_inventory(currency_class, amount)
        return True
    except Exception as exc:
        bd.warn(f"bd_npcs: payment failed: {exc!r}")
        return False


def _play_trade_sound() -> None:
    """Transaction feedback through the UI sound path, guarded."""
    try:
        bd.play_ui_sound(_TRADE_SOUND)
    except Exception:
        pass


class Service:
    """A named service an NPC offers (healing, training, ...).

    ``available(npc_id, ctx)`` gates whether the offer is made and
    ``run(npc_id, ctx)`` performs it, returning a dict of the shape
    ``{"ok": bool, "message": str}`` for HUD/UI feedback. The ``ctx``
    dict carries ``pawn``, ``character``, ``dispositions``, ``manager``,
    ``shop``, and arbitrary mod keys (see the module docstring).
    Subclasses override both methods; the base service is a stub that
    refuses politely.
    """

    def __init__(self, name: str = "Service") -> None:
        self.name: str = str(name)

    def available(self, npc_id: str, ctx: Dict[str, Any]) -> bool:
        """True when the service may be offered right now (default True)."""
        return True

    def run(self, npc_id: str, ctx: Dict[str, Any]) -> Dict[str, Any]:
        """Perform the service; returns {"ok": bool, "message": str}."""
        return {"ok": False,
                "message": f"{self.name} has nothing to offer."}


class HealerService(Service):
    """Heal the player pawn (and a bd_dnd character's RPG hp).

    ``amount`` None heals to full; otherwise both the pawn (through
    ``Actor.heal``) and a ``character`` from the ctx (through
    ``Character.set_hp``, so hp-changed hooks fire) are healed by that
    many points. ``cost`` currency units are paid through the shared
    :func:`_pay` helper before anything is healed.

    The service refuses (``ok`` False with a message) when the payer
    cannot afford the cost or the NPC's standing ranks below
    ``min_standing`` (a :data:`~bd_npcs.disposition.STANDINGS` name,
    compared by index; default ``"cold"`` means a hostile NPC refuses).
    With ``cost=0`` and ``min_standing=None`` the service is free.
    """

    #: Heal amount used for a full heal (the native path clamps to max).
    FULL_HEAL = 1000000

    def __init__(self, name: str = "Healer", amount: Optional[int] = None,
                 cost: int = 0, currency_class: str = "Coin",
                 min_standing: Optional[str] = "cold") -> None:
        super().__init__(name)
        if amount is not None and int(amount) <= 0:
            raise ValueError("bd_npcs: heal amount must be positive or "
                             "None")
        if int(cost) < 0:
            raise ValueError("bd_npcs: cost must not be negative")
        if min_standing is not None and min_standing not in STANDINGS:
            raise ValueError(f"bd_npcs: min_standing must be one of "
                             f"{STANDINGS} or None")
        self.amount: Optional[int] = (None if amount is None
                                      else int(amount))
        self.cost: int = int(cost)
        self.currency_class: str = str(currency_class)
        self.min_standing: Optional[str] = min_standing

    def _standing_allows(self, npc_id: str, ctx: Dict[str, Any]) -> bool:
        if self.min_standing is None:
            return True
        dispositions = ctx.get("dispositions")
        if dispositions is None:
            return True
        try:
            standing = dispositions.standing(npc_id)
            return STANDINGS.index(standing) >= \
                STANDINGS.index(self.min_standing)
        except Exception as exc:
            bd.warn(f"bd_npcs: disposition check failed: {exc!r}")
            return False

    def available(self, npc_id: str, ctx: Dict[str, Any]) -> bool:
        if not self._standing_allows(npc_id, ctx):
            return False
        pawn = ctx.get("pawn")
        if self.cost > 0 and pawn is not None:
            try:
                return int(pawn.inventory_count(self.currency_class)) \
                    >= self.cost
            except Exception:
                return False
        return True

    def run(self, npc_id: str, ctx: Dict[str, Any]) -> Dict[str, Any]:
        pawn = ctx.get("pawn")
        character = ctx.get("character")
        if pawn is None and character is None:
            return {"ok": False,
                    "message": f"{self.name}: there is no patient."}
        if not self._standing_allows(npc_id, ctx):
            dispositions = ctx.get("dispositions")
            standing = (dispositions.standing(npc_id)
                        if dispositions is not None else "cold")
            return {"ok": False,
                    "message": f"{self.name}: the healer wants nothing to "
                               f"do with you ({standing})."}
        if self.cost > 0:
            if not _pay(pawn, self.currency_class, self.cost):
                return {"ok": False,
                        "message": f"{self.name}: you cannot afford the "
                                   f"{self.cost} {self.currency_class} "
                                   f"fee."}
        healed = False
        if pawn is not None:
            try:
                amount = (self.FULL_HEAL if self.amount is None
                          else self.amount)
                healed = bool(pawn.heal(amount)) or healed
            except Exception as exc:
                bd.warn(f"bd_npcs: pawn heal failed: {exc!r}")
        if character is not None:
            try:
                if self.amount is None:
                    before = int(character.hp)
                    character.set_hp(int(character.max_hp))
                    healed = int(character.hp) > before or healed
                else:
                    before = int(character.hp)
                    character.set_hp(before + self.amount)
                    healed = int(character.hp) > before or healed
            except Exception as exc:
                bd.warn(f"bd_npcs: character heal failed: {exc!r}")
        if self.amount is None:
            message = f"{self.name}: restored to full health."
        else:
            message = f"{self.name}: healed {self.amount}."
        if not healed:
            message = f"{self.name}: nothing needed healing."
        return {"ok": True, "message": message}


class TrainerService(Service):
    """Train a bd_dnd skill through use-based mastery advancement.

    Each :meth:`run` performs up to ``uses_per_check``
    ``bd_dnd.classes.advancement_check`` rolls for ``skill`` (the
    bd_dnd module is imported lazily, keeping bd_npcs import-time
    dependency-free); every improvement raises the mastery tier and is
    reported in the message. ``cost`` currency units are paid when the
    training proceeds (before the rolls). The service refuses when the
    ctx carries no ``character`` or the skill's mastery already reached
    ``max_mastery``.
    """

    def __init__(self, skill: str, cost: int = 0, max_mastery: int = 3,
                 uses_per_check: int = 1, name: Optional[str] = None,
                 currency_class: str = "Coin") -> None:
        skill = str(skill)
        if not skill.strip():
            raise ValueError("bd_npcs: TrainerService needs a skill name")
        super().__init__(name if name is not None
                         else f"Train {skill.replace('_', ' ')}")
        if int(cost) < 0:
            raise ValueError("bd_npcs: cost must not be negative")
        if int(max_mastery) < 1:
            raise ValueError("bd_npcs: max_mastery must be at least 1")
        if int(uses_per_check) < 1:
            raise ValueError("bd_npcs: uses_per_check must be at least 1")
        self.skill: str = skill
        self.cost: int = int(cost)
        self.max_mastery: int = int(max_mastery)
        self.uses_per_check: int = int(uses_per_check)
        self.currency_class: str = str(currency_class)

    @staticmethod
    def _classes() -> Any:
        """The bd_dnd.classes module (late import by design)."""
        from bd_dnd import classes as dnd_classes
        return dnd_classes

    def available(self, npc_id: str, ctx: Dict[str, Any]) -> bool:
        character = ctx.get("character")
        if character is None:
            return False
        try:
            return self._classes().mastery(character, self.skill) \
                < self.max_mastery
        except Exception as exc:
            bd.warn(f"bd_npcs: mastery check failed: {exc!r}")
            return False

    def run(self, npc_id: str, ctx: Dict[str, Any]) -> Dict[str, Any]:
        character = ctx.get("character")
        if character is None:
            return {"ok": False,
                    "message": f"{self.name}: there is no student."}
        dnd_classes = self._classes()
        try:
            mastery = dnd_classes.mastery(character, self.skill)
        except Exception as exc:
            bd.warn(f"bd_npcs: mastery check failed: {exc!r}")
            return {"ok": False,
                    "message": f"{self.name}: could not train "
                               f"{self.skill} ({exc})."}
        if mastery >= self.max_mastery:
            return {"ok": False,
                    "message": f"{self.name}: {self.skill} is already "
                               f"mastered."}
        pawn = ctx.get("pawn")
        if self.cost > 0:
            if not _pay(pawn, self.currency_class, self.cost):
                return {"ok": False,
                        "message": f"{self.name}: you cannot afford the "
                                   f"{self.cost} {self.currency_class} "
                                   f"fee."}
        rng = ctx.get("rng") or bd
        improvements = 0
        last_roll: Any = None
        for _ in range(self.uses_per_check):
            try:
                result = dnd_classes.advancement_check(
                    character, self.skill, rng=rng)
            except Exception as exc:
                bd.warn(f"bd_npcs: advancement check failed: {exc!r}")
                break
            if isinstance(result, dict):
                last_roll = result.get("roll")
                if result.get("improved"):
                    improvements += 1
        if improvements:
            message = (f"{self.name}: {self.skill} improved "
                       f"{improvements}x (mastery "
                       f"{mastery + improvements}/{self.max_mastery}).")
        else:
            message = (f"{self.name}: no progress on {self.skill} "
                       f"(rolled {last_roll}).")
        return {"ok": True, "message": message}


class Shop:
    """A currency-based shop with stock counts and restock timers.

    ``stock`` entries are dicts with the keys::

        {"class_name": str, "name": str, "price": int,
         "sell_ratio": float (default 0.5), "max_count": int (default 99),
         "restock_tics": int (default 0 = never restocks)}

    Missing keys are filled with defaults; a bad entry raises
    ``ValueError``. ``sellables`` names the item classes the shop buys
    back, the default empty tuple means it buys anything the player
    carries (unstocked classes at :data:`UNLISTED_SELL_PRICE`). Stock
    counts start at ``max_count`` and persist through
    :meth:`stock_snapshot`/:meth:`restore` (counts only, keyed by class
    name); definitions are never persisted.

    Buy/sell methods return ``{"ok": bool, "message": str, "price": int}``
    and play a UI sound on success (guarded). Buying takes the currency
    first and refunds it when the item cannot be delivered (delivery is
    verified by the before/after ``inventory_count`` delta, because
    ``give_inventory`` reports the actor's total, not the amount given);
    when ``restock_tics`` is positive a one-shot map-agnostic
    ``bd.schedule`` task restores the count to ``max_count`` — restock
    timers survive map changes because they touch only this Shop's own
    index-based counts.
    """

    def __init__(self, currency_class: str = "Coin",
                 currency_name: str = "Gold", stock: Any = None,
                 sellables: Any = ()) -> None:
        self.currency_class: str = str(currency_class)
        self.currency_name: str = str(currency_name)
        self.stock: List[Dict[str, Any]] = []
        self._counts: List[int] = []
        for entry in (stock or ()):
            normalized = self._normalize_entry(entry)
            self.stock.append(normalized)
            self._counts.append(normalized["max_count"])
        self.sellables: Tuple[str, ...] = tuple(
            str(item) for item in (sellables or ()))

    # -- stock shape -----------------------------------------------------------

    @staticmethod
    def _normalize_entry(entry: Any) -> Dict[str, Any]:
        """Validate one stock entry dict, filling the documented defaults."""
        if not isinstance(entry, dict):
            raise ValueError(f"bd_npcs: stock entries must be dicts, got "
                             f"{entry!r}")
        class_name = str(entry.get("class_name", "")).strip()
        if not class_name:
            raise ValueError("bd_npcs: stock entries need a 'class_name'")
        try:
            price = int(entry.get("price", 0))
            max_count = int(entry.get("max_count", 99))
            restock_tics = int(entry.get("restock_tics", 0))
            sell_ratio = float(entry.get("sell_ratio", 0.5))
        except (TypeError, ValueError) as exc:
            raise ValueError(f"bd_npcs: bad stock entry {entry!r}: {exc}")
        if price < 0 or max_count < 0 or restock_tics < 0:
            raise ValueError(f"bd_npcs: stock numbers must not be "
                             f"negative: {entry!r}")
        if not 0.0 <= sell_ratio <= 1.0:
            raise ValueError(f"bd_npcs: sell_ratio must be 0.0..1.0: "
                             f"{entry!r}")
        return {"class_name": class_name,
                "name": str(entry.get("name", class_name)),
                "price": price,
                "sell_ratio": sell_ratio,
                "max_count": max_count,
                "restock_tics": restock_tics}

    def _entry_for(self, class_name: str) -> Optional[Dict[str, Any]]:
        """The stock entry matching ``class_name`` (case-insensitive)."""
        lowered = str(class_name).lower()
        for entry in self.stock:
            if entry["class_name"].lower() == lowered:
                return entry
        return None

    # -- pricing -----------------------------------------------------------------

    def price_of(self, entry: Any) -> int:
        """The int price of a stock entry (accepted as dict or index)."""
        if isinstance(entry, int) and not isinstance(entry, bool):
            entry = self.stock[entry]
        return int(entry.get("price", 0))

    def sell_price_of(self, class_name: str) -> int:
        """The int price the shop pays for one ``class_name`` unit."""
        entry = self._entry_for(class_name)
        if entry is not None:
            return int(entry["price"] * entry["sell_ratio"])
        return UNLISTED_SELL_PRICE

    def currency(self, pawn: Any) -> int:
        """The currency units ``pawn`` carries (0 on any failure)."""
        try:
            return int(pawn.inventory_count(self.currency_class))
        except Exception as exc:
            bd.warn(f"bd_npcs: currency query failed: {exc!r}")
            return 0

    def count_of(self, index: Any) -> int:
        """Units remaining in stock at ``index`` (0 when out of range)."""
        try:
            return self._counts[int(index)]
        except (IndexError, TypeError, ValueError):
            return 0

    # -- transactions ------------------------------------------------------------

    def buy(self, pawn: Any, index: Any) -> Dict[str, Any]:
        """Buy one unit of stock ``index``; returns ok/message/price.

        Refuses (ok False, nothing moved) when the stock is empty or the
        pawn cannot afford the item. Takes the currency first and
        refunds it when the item cannot be delivered (verified by the
        before/after ``inventory_count`` delta). On success the count
        decrements and, when the entry's ``restock_tics`` is positive, a
        one-shot map-agnostic task restores the count to ``max_count``.
        """
        index = int(index)
        if not 0 <= index < len(self.stock):
            raise ValueError(f"bd_npcs: shop index {index} out of range "
                             f"({len(self.stock)} entries)")
        entry = self.stock[index]
        name = entry["name"]
        price = self.price_of(entry)
        if self._counts[index] <= 0:
            return {"ok": False, "message": f"{name} is out of stock.",
                    "price": price}
        if self.currency(pawn) < price:
            return {"ok": False,
                    "message": f"You cannot afford {name} ({price} "
                               f"{self.currency_name}).",
                    "price": price}
        try:
            pawn.take_inventory(self.currency_class, price)
        except Exception as exc:
            return {"ok": False,
                    "message": f"Payment failed: {exc}.",
                    "price": price}
        # Delivery is measured by the before/after inventory_count delta:
        # give_inventory returns the actor's TOTAL amount of the class
        # after the give, so its return value cannot distinguish "one
        # delivered" from "already carrying some, give silently refused"
        # (e.g. the item is at its max amount).
        given = 0
        try:
            before = int(pawn.inventory_count(entry["class_name"]))
            pawn.give_inventory(entry["class_name"], 1)
            given = int(pawn.inventory_count(entry["class_name"])) - before
        except ValueError as exc:
            # Unknown item class is a mod bug: refund and report.
            try:
                pawn.give_inventory(self.currency_class, price)
            except Exception:
                pass
            return {"ok": False,
                    "message": f"Cannot stock {name}: {exc}.",
                    "price": price}
        except Exception as exc:
            try:
                pawn.give_inventory(self.currency_class, price)
            except Exception:
                pass
            return {"ok": False,
                    "message": f"Transaction failed: {exc}.",
                    "price": price}
        if given <= 0:
            try:
                pawn.give_inventory(self.currency_class, price)
            except Exception:
                pass
            return {"ok": False,
                    "message": f"Could not deliver {name}; refunded.",
                    "price": price}
        self._counts[index] -= 1
        if entry["restock_tics"] > 0:
            # The restock closure touches only this Shop's own index-based
            # counts (no actor handles), so it is deliberately map-agnostic
            # (map_local=False): a restock timer must keep ticking across
            # hub transitions and regular map changes alike.
            try:
                bd.schedule(lambda i=index: self._restock(i),
                            delay=entry["restock_tics"], map_local=False)
            except Exception as exc:
                bd.warn(f"bd_npcs: restock scheduling failed: {exc!r}")
        _play_trade_sound()
        return {"ok": True,
                "message": f"Bought {name} for {price} "
                           f"{self.currency_name}.",
                "price": price}

    def sell(self, pawn: Any, class_name: str) -> Dict[str, Any]:
        """Sell one ``class_name`` unit; returns ok/message/price.

        Refuses when the shop does not buy the class (a non-empty
        ``sellables`` limits what it buys) or the pawn carries none.
        The price is the matching stock entry's ``price * sell_ratio``,
        or :data:`UNLISTED_SELL_PRICE` for classes the shop does not
        stock (only reachable when ``sellables`` is empty or lists the
        class).
        """
        class_name = str(class_name)
        if self.sellables and class_name not in self.sellables:
            return {"ok": False,
                    "message": f"The shop is not buying {class_name}.",
                    "price": 0}
        try:
            held = int(pawn.inventory_count(class_name))
        except Exception as exc:
            return {"ok": False,
                    "message": f"Inventory query failed: {exc}.",
                    "price": 0}
        if held <= 0:
            return {"ok": False,
                    "message": f"You have no {class_name} to sell.",
                    "price": 0}
        price = self.sell_price_of(class_name)
        try:
            pawn.take_inventory(class_name, 1)
            pawn.give_inventory(self.currency_class, price)
        except Exception as exc:
            return {"ok": False,
                    "message": f"Transaction failed: {exc}.",
                    "price": price}
        _play_trade_sound()
        return {"ok": True,
                "message": f"Sold {class_name} for {price} "
                           f"{self.currency_name}.",
                "price": price}

    def _restock(self, index: Any) -> None:
        """Restore the count at ``index`` to its ``max_count``."""
        try:
            self._counts[int(index)] = self.stock[int(index)]["max_count"]
        except Exception as exc:
            bd.warn(f"bd_npcs: restock failed: {exc!r}")

    # -- persistence ---------------------------------------------------------------

    def stock_snapshot(self) -> Dict[str, Any]:
        """Plain JSON-able stock counts, keyed by class name."""
        return {"version": 1,
                "counts": {entry["class_name"]: self._counts[i]
                           for i, entry in enumerate(self.stock)}}

    def restore(self, data: Any) -> None:
        """Tolerant restore of a :meth:`stock_snapshot` payload."""
        if not isinstance(data, dict):
            return
        counts = data.get("counts", data)
        if not isinstance(counts, dict):
            return
        by_class = {entry["class_name"].lower(): i
                    for i, entry in enumerate(self.stock)}
        for class_name, count in counts.items():
            index = by_class.get(str(class_name).lower())
            if index is None:
                continue
            try:
                count = int(count)
            except (TypeError, ValueError):
                continue
            self._counts[index] = max(
                0, min(self.stock[index]["max_count"], count))

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<Shop currency={self.currency_class!r} "
                f"entries={len(self.stock)} sellables={len(self.sellables)}>")


class ShopUI:
    """Immediate-mode ImGui window rendering a :class:`Shop`.

    Draw only: call :meth:`draw` from an ``imgui_frame`` handler, like
    ``bd_dnd.sheet.CharacterSheet``. The window starts visible; the
    close button and :meth:`toggle` stay in sync because ``begin``'s
    open result is fed back into :attr:`visible` every frame. Guarded:
    a rendering error produces one ``bd.warn`` per frame at worst and
    always balances ``begin`` with ``end``; the window is inert while
    collapsed.

    The window shows the currency line, a stock table (name, price,
    count, and a Buy button per row, disabled when unaffordable or out
    of stock), and a sell section listing the shop's explicit
    ``sellables`` classes the player actually carries (count queried
    per class; the engine offers no generic inventory listing). The
    last transaction message renders as a muted line.
    """

    def __init__(self, shop: Shop, title: str = "Shop") -> None:
        self.shop: Shop = shop
        self.title: str = str(title)
        self.visible: bool = True
        self._message: str = ""

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the shop window. Call from an ``imgui_frame`` handler."""
        if not self.visible:
            return
        imgui = bd.imgui
        try:
            imgui.set_next_window_pos(420.0, 60.0,
                                      imgui.Cond.FirstUseEver)
            imgui.set_next_window_size(360.0, 0.0,
                                       imgui.Cond.FirstUseEver)
            expanded, self.visible = imgui.begin(
                f"{self.title}###bd_npcs_shop", self.visible)
            try:
                if expanded:
                    self._draw_contents(imgui)
            except Exception as exc:
                bd.warn(f"bd_npcs shop draw error: {exc!r}")
            finally:
                imgui.end()
        except Exception as exc:
            bd.warn(f"bd_npcs shop window error: {exc!r}")

    # -- internals --------------------------------------------------------------

    def _apply(self, result: Any) -> None:
        """Record a transaction message for the muted status line."""
        if isinstance(result, dict) and result.get("message"):
            self._message = str(result["message"])

    def _draw_contents(self, imgui: Any) -> None:
        pawn = _player_pawn()
        if pawn is None:
            imgui.text_disabled("No player.")
            return
        shop = self.shop
        imgui.text(f"{shop.currency_name}: {shop.currency(pawn)}")
        if self._message:
            imgui.text_disabled(self._message)
        imgui.separator()
        if imgui.begin_table("bd_npcs_shop_stock", 4):
            try:
                imgui.table_setup_column("Item")
                imgui.table_setup_column("Price")
                imgui.table_setup_column("Stock")
                imgui.table_setup_column("")
                imgui.table_headers_row()
                for index, entry in enumerate(shop.stock):
                    price = shop.price_of(entry)
                    count = shop.count_of(index)
                    imgui.table_next_row()
                    imgui.table_next_column()
                    imgui.text(entry["name"])
                    imgui.table_next_column()
                    imgui.text(str(price))
                    imgui.table_next_column()
                    imgui.text(str(count))
                    imgui.table_next_column()
                    affordable = count > 0 and \
                        shop.currency(pawn) >= price
                    if not affordable:
                        imgui.text_disabled("Buy")
                    elif imgui.button(f"Buy##bd_npcs_buy_{index}"):
                        self._apply(shop.buy(pawn, index))
            finally:
                imgui.end_table()
        imgui.separator()
        imgui.text_disabled("Sell")
        if not shop.sellables:
            imgui.text_disabled("This shop buys nothing.")
            return
        any_row = False
        for class_name in shop.sellables:
            try:
                held = int(pawn.inventory_count(class_name))
            except Exception:
                held = 0
            if held <= 0:
                continue
            any_row = True
            price = shop.sell_price_of(class_name)
            imgui.text(f"{class_name} x{held} (+{price} "
                       f"{shop.currency_name})")
            imgui.same_line()
            if imgui.button(f"Sell##bd_npcs_sell_{class_name}"):
                self._apply(shop.sell(pawn, class_name))
        if not any_row:
            imgui.text_disabled("Nothing the shop buys.")

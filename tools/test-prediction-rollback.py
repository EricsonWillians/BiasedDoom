#!/usr/bin/env python3
"""Source contracts for prediction rollback actor-list lifetime rules.

The player prediction path uses an intentionally low-level sector-list
snapshot. These checks preserve two compatibility invariants that are difficult
to exercise in a headless gameplay fixture: a client-side actor that predates
prediction must be restored, while a client-side actor created during
prediction must be discarded. The same audit covers the map-scoped diagnostic
limiter used by high-rate third-party visual effects.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def between(source: str, start: str, end: str) -> str:
    start_index = source.find(start)
    assert start_index >= 0, f"missing start marker: {start!r}"
    end_index = source.find(end, start_index)
    assert end_index >= 0, f"missing end marker: {end!r}"
    return source[start_index:end_index]


def main() -> None:
    player_source = (ROOT / "src/playsim/p_user.cpp").read_text()
    object_source = (ROOT / "src/common/objects/dobject.cpp").read_text()
    level_header = (ROOT / "src/g_levellocals.h").read_text()
    level_source = (ROOT / "src/g_level.cpp").read_text()
    tarray_source = (ROOT / "src/common/utility/tarray.h").read_text()

    predict_scope = between(
        player_source,
        "void P_PredictPlayer (player_t *player)",
        "void P_UnPredictPlayer ()",
    )
    unpredict_scope = between(
        player_source,
        "void P_UnPredictPlayer ()",
        "void player_t::Serialize",
    )
    assert "PredictionSectorListMembers.Insert(link, PSM_Snapshot);" in predict_scope

    restore_scope = between(
        unpredict_scope,
        "TArray<AActor *> lateSectorActors;",
        "// Only the touching list actually needs to be restored",
    )
    assert "membership = &PredictionSectorListMembers.Insert(me, PSM_Current);" in restore_scope
    assert "*membership |= PSM_Current;" in restore_scope
    assert "const bool wasSnapshotActor = (*membership & PSM_Snapshot) != 0;" in restore_scope
    assert "(wasSnapshotActor || !me->IsClientSide())" in restore_scope
    assert "else if (me != act && !wasSnapshotActor)" in restore_scope

    snapshot_restore = between(
        restore_scope,
        "for (i = PredictionSectorListBackup.Size(); i-- > 0;)",
        "// Newly-created actors were at the head",
    )
    assert "me->IsClientSide()" not in snapshot_restore
    assert "PredictionSectorListMembers.Remove(" not in player_source
    final_clear = unpredict_scope.rfind("ClearPredictionSectorListBackup();")
    assert final_clear > unpredict_scope.find("for (i = PredictionSectorListBackup.Size(); i-- > 0;)")
    cache_clear = between(
        player_source,
        "static void ClearPredictionSectorListBackup()",
        "static TArray<sector_t *> PredictionTouchingSectorsBackup;",
    )
    assert "PredictionSectorListMembers.ClearKeepCapacity();" in cache_clear

    clear_keep_capacity = between(
        tarray_source,
        "void ClearKeepCapacity()",
        "//=======================================================================\n\t//\n\t// CountUsed",
    )
    assert "Nodes[i].~Node();" in clear_keep_capacity
    assert "Nodes[i].SetNil();" in clear_keep_capacity
    assert "LastFree = &Nodes[Size];" in clear_keep_capacity
    assert "NumUsed = 0;" in clear_keep_capacity

    assert 'P_ReportPredictionObjectWarning("Destroyed non-client-side Object"' in object_source
    assert 'P_ReportPredictionObjectWarning("Spawned non-client-side Thinker"' in level_header
    assert "PredictionObjectWarningCount" in level_header
    assert "PredictionObjectWarningsSuppressed" in level_header
    assert "MaxPredictionObjectWarnings = 8" in level_source
    assert "Further non-client-side object warnings are suppressed for this map" in level_source

    print("PASS: prediction rollback preserves snapshot client-side actors and bounds diagnostics")


if __name__ == "__main__":
    main()

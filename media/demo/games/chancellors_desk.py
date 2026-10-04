"""The Chancellor's Desk — a political drama at one desk, on one rainy night in a new republic.

Original IP. Mirovan, November 1946: the Meridian War is over, the Directorate has fallen and
the Republic of Kestria has its first elected Chancellor. Tonight the Assembly sends up the
Bridges Decree. Three advisors, one newspaper, one signature.

The office is a cinematic interior: a walnut pedestal desk (Blender) under a green banker's lamp,
Poly Haven furniture and photoscanned materials, the republic's flags, velvet curtains and a tall
window onto the rainy city at dusk; cool window light in volumetric haze against the warm lamp,
with depth of field. The UI is a restyled dialogue with advisor cameo portraits and branching
choices that move approval, stability and the treasury; a newspaper with its own masthead; and
the decree (the restyled `document` template) with Sign and Veto.
"""
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import polyhaven as ph  # noqa: E402

META = dict(
    id="chancellors_desk", title="The Chancellor's Desk", genre="Narrative Political Drama", mood="rain, lamplight, consequence",
    pitch="One desk. One rainy night. The republic is three weeks old and the pen is in your hand.",
    assets="Poly Haven (CC0) furniture, props and photoscanned materials; Blender-modeled desk, lamp and flags; procedural art",
    startScene="scenes/main.sky.json",
    include=["art/*", "art/portraits/*.png", "ui/*", "story/*"],
)

ART = "art"
OFFICE = "generated/office"
# Room: x across (-4.5..4.5), z from the window wall (-3.8) to the door wall (4.5), y up (0..4.2).
RW, RZ0, RZ1, RH = 4.5, -3.8, 4.5, 4.2
WIN_X, WIN_Y0, WIN_Y1 = 1.4, 0.75, 3.65
DESK = (0.0, -1.9)
TOP = 0.782   # desk top height

UI_STYLE = {
    "format": "skywalker.uistyle", "extends": "dark",
    "vars": {"ink": "#ece2cc", "muted": "#a89d88", "gold": "#cfa75a", "paper": "#efe6d2", "red": "#9e2a22"},
    "rules": {
        "canvas": {"font": "serif", "fontSize": 22, "color": "$ink"},
        ".hud": {"background": "#120f0cd8", "background2": "#0b0908e0", "borderWidth": 1, "borderColor": "#cfa75a44", "radius": 2,
                 "padding": [10, 16], "shadowColor": "#00000090", "shadowOffset": [0, 6], "shadowBlur": 22},
        ".cap": {"font": "Inter", "fontSize": 12, "letterSpacing": 0.2, "textTransform": "uppercase", "color": "$muted"},
        ".stat": {"font": "serif", "fontSize": 26, "color": "#f4ead2"},
        ".delta_up": {"font": "Inter", "fontSize": 13, "color": "#8fc98a"},
        ".delta_down": {"font": "Inter", "fontSize": 13, "color": "#e07a62"},
        "progress": {"accent": "#cfa75a", "track": "#ffffff16", "trackHeight": 5, "radius": 2},
        ".bar_red": {"accent": "#c0503c"},
        ".bar_blue": {"accent": "#7c9cc8"},
        # dialogue: a dark lacquered box, a cameo, Garamond lines and choices like typed memoranda
        ".dialogue_box": {"background": "#14100cf2", "background2": "#0c0a08f6", "radius": 2, "borderWidth": 1,
                          "borderColor": "#cfa75a66", "shadowColor": "#000000d0", "shadowOffset": [0, 14], "shadowBlur": 48},
        ".dialogue_portrait": {"radius": 2, "borderWidth": 1, "borderColor": "#cfa75a88", "imageFit": "cover", "background": "#00000040"},
        ".dialogue_name": {"font": "Inter", "fontSize": 15, "bold": True, "color": "#cfa75a", "letterSpacing": 0.24,
                           "textTransform": "uppercase"},
        ".dialogue_text": {"font": "serif", "fontSize": 30, "lineSpacing": 1.28, "color": "#efe5cf", "verticalAlign": "top"},
        ".dialogue_choice": {"background": "#efe6d2f4", "background2": "#e2d6bcf4", "borderWidth": 1, "borderColor": "#7a5a34",
                             "radius": 1, "font": "serif", "fontSize": 23, "color": "#2a2018", "textAlign": "left",
                             "padding": [10, 18], "shadowColor": "#00000080", "shadowOffset": [0, 4], "shadowBlur": 12,
                             "hover": {"borderColor": "#9e2a22", "background": "#fff6e2f8"}},
        ".dialogue_hint": {"font": "Inter", "fontSize": 12, "color": "#8a7f6c", "textAlign": "right"},
        # documents
        ".sheet": {"backgroundImage": "art/paper.jpg", "imageFit": "cover", "radius": 1, "shadowColor": "#000000c8",
                   "shadowOffset": [0, 18], "shadowBlur": 50, "color": "#2a221c"},
        ".newsprint": {"backgroundImage": "art/newsprint.jpg", "imageFit": "cover", "radius": 1, "shadowColor": "#000000c8",
                       "shadowOffset": [0, 18], "shadowBlur": 50, "color": "#1c1a18"},
        ".masthead": {"font": "serif", "fontSize": 76, "color": "#1c1a18", "textAlign": "center"},
        ".dateline": {"font": "Inter", "fontSize": 12, "letterSpacing": 0.14, "textTransform": "uppercase", "color": "#3a3632"},
        ".headline": {"font": "serif", "fontSize": 44, "bold": True, "color": "#1c1a18", "textAlign": "center", "lineSpacing": 1.05},
        ".deck": {"font": "serif", "fontSize": 21, "italic": True, "color": "#3a3632", "textAlign": "center"},
        ".column": {"font": "serif", "fontSize": 17, "lineSpacing": 1.32, "color": "#24201c", "textAlign": "justify"},
        ".caption": {"font": "serif", "fontSize": 14, "italic": True, "color": "#4a4440"},
        ".doc_head": {"font": "Inter", "fontSize": 13, "letterSpacing": 0.3, "textTransform": "uppercase", "color": "#6a5038",
                      "textAlign": "center"},
        ".doc_title": {"font": "serif", "fontSize": 46, "color": "#241c16", "textAlign": "center"},
        ".doc_sub": {"font": "serif", "fontSize": 24, "italic": True, "color": "#4a3a2c", "textAlign": "center"},
        ".doc_body": {"font": "serif", "fontSize": 22, "lineSpacing": 1.38, "color": "#2a221c"},
        ".sign": {"background": "#244a32", "background2": "#1a3624", "borderColor": "#cfa75a", "borderWidth": 1, "color": "#f4ead2",
                  "font": "Inter", "fontSize": 15, "letterSpacing": 0.2, "textTransform": "uppercase", "radius": 1,
                  "padding": [12, 28], "hover": {"background": "#2e6040"}},
        ".veto": {"background": "#00000000", "borderColor": "#9e2a22", "borderWidth": 1, "color": "#9e2a22", "font": "Inter",
                  "fontSize": 15, "letterSpacing": 0.2, "textTransform": "uppercase", "radius": 1, "padding": [12, 28],
                  "hover": {"background": "#9e2a2218"}},
    },
}

DIALOGUE = """title: Start
---
<<declare $approval = 52>>
<<declare $stability = 61>>
<<declare $treasury = 124>>
<<declare $army = 40>>
Dornach: Chancellor. The Assembly passed it. Two hundred and twelve to one hundred and ninety-eight. #portrait:dornach
Dornach: The Bridges Decree is on your desk. The Ledger already has the vote on its front page. #portrait:dornach
<<show_paper>>
-> Fourteen votes. That is not a mandate, Ilse.
    Dornach: It is the only mandate this republic has ever had. #portrait:dornach
-> Who is waiting outside?
    Dornach: General Vass. And Ardanne from the Treasury. They arrived together, which worries me. #portrait:dornach
<<hide_paper>>
<<jump Treasury>>
===
title: Treasury
---
Ardanne: Nine hundred million crowns, Chancellor. The Bond will not sell at four percent. Not this winter. #portrait:ardanne
-> Then we sell it at six.
    <<set $treasury -= 18>>
    <<set $approval += 3>>
    Ardanne: Then we will be paying for these bridges until my grandchildren retire. #portrait:ardanne
-> Cut the contracts in the eastern provinces.
    <<set $treasury += 22>>
    <<set $stability -= 9>>
    Ardanne: The east will remember who drew the line through their villages. #portrait:ardanne
<<jump General>>
===
title: General
---
Vass: The Engineer Corps built those bridges, and the Corps blew them up. We will not rebuild them under clerks. #portrait:vass
-> The Corps answers to the Republic now, General.
    <<set $army -= 15>>
    <<set $approval += 4>>
    Vass: Then the Republic had better learn to pour concrete. #portrait:vass
-> Command of the works stays with you. On paper it is civil.
    <<set $army += 20>>
    <<set $stability += 4>>
    <<set $approval -= 6>>
    Vass: A sensible arrangement. The men will be pleased. #portrait:vass
Dornach: The decree, Chancellor. They are waiting for the ink. #portrait:dornach
<<show_decree>>
===
"""

CAMPAIGN = """behavior Chancellery
  intent "The night at the desk: the briefing starts, advisors' choices move approval, stability and the treasury, the newspaper and the decree come up when the dialogue calls for them, and Sign or Veto stamps the decree and settles the numbers."
  var approval = 52
  var stability = 61
  var treasury = 124
  var decided = false
  var shown_a = 52.0
  var shown_s = 61.0
  var shown_t = 124.0
  var autoplay = true
  var talk_t = 0
  var film = false
  var opened = false

  fn refresh()
    shown_a = lerp(shown_a, approval, 0.08)
    shown_s = lerp(shown_s, stability, 0.08)
    shown_t = lerp(shown_t, treasury, 0.08)
    find("Approval").ui.text = str(round(shown_a)) + "%"
    find("Approval Bar").ui.value = shown_a / 100
    find("Stability").ui.text = str(round(shown_s)) + "%"
    find("Stability Bar").ui.value = shown_s / 100
    find("Treasury").ui.text = "Kr " + str(round(shown_t / 100, 2)) + "bn"
    find("Treasury Bar").ui.value = shown_t / 250
  end

  fn pull()
    approval = dialogue_var("approval")
    stability = dialogue_var("stability")
    treasury = dialogue_var("treasury")
  end

  on start
    find("Newspaper").ui.visible = false
    find("Document").ui.visible = false
    find("Stamp").ui.visible = false
    find("Cursor").ui.visible = false
  end

  on tick
    refresh()
    -- in play the briefing opens by itself; a film shot (its first event) takes over instead
    if not opened and not film and time > 1.5 then
      opened = true
      start_dialogue(find("Advisors"), "Start")
    end
    let adv = find("Advisors")
    if autoplay and adv.dialogue.running then
      talk_t += dt
      if talk_t > 3.2 then
        talk_t = 0
        if adv.dialogue.choices.length > 0 then
          dialogue_choose(0)
        else
          dialogue_advance()
        end
      end
    end
  end

  on key "space"
    autoplay = false
  end

  on event "briefing"
    start_dialogue(find("Advisors"), "Start")
  end
  on event "briefing_general"
    start_dialogue(find("Advisors"), "General")
  end
  on event "no_autoplay"
    autoplay = false
  end
  on dialogue "choice"
    pull()
  end
  on dialogue "line"
    pull()
  end
  on dialogue "show_paper"
    find("Newspaper").ui.visible = true
  end
  on dialogue "hide_paper"
    find("Newspaper").ui.visible = false
  end
  on dialogue "show_decree"
    find("Document").ui.visible = true
  end
  on event "paper_show"
    find("Newspaper").ui.visible = true
  end
  on event "paper_hide"
    find("Newspaper").ui.visible = false
  end
  on event "decree_show"
    find("Document").ui.visible = true
  end
  on event "decree_hide"
    find("Document").ui.visible = false
  end
  on event "hud_hide"
    film = true
    find("Office UI").enabled = false
  end
  on event "hud_show"
    film = true
    find("Office UI").enabled = true
  end
  on event "stats_hide"
    find("Stats").ui.visible = false
    find("Dateline").ui.visible = false
  end
  on ui "Sign"
    if not decided then
      decided = true
      approval += 6
      stability += 5
      treasury -= 31
      find("Stamp").ui.image = "art/stamp_signed.png"
      find("Stamp").ui.visible = true
      find("Sign").ui.interactable = false
      find("Veto").ui.interactable = false
    end
  end
  on event "sign"
    emit "ui:Sign"
  end
  on ui "Veto"
    if not decided then
      decided = true
      approval -= 9
      stability -= 7
      find("Stamp").ui.image = "art/stamp_vetoed.png"
      find("Stamp").ui.visible = true
      find("Sign").ui.interactable = false
      find("Veto").ui.interactable = false
    end
  end
end"""

LAMP = """behavior Lamplight
  intent "The banker's lamp breathes a little, like an old filament on unsteady wartime current."
  var base = 0
  on start
    base = self.light.intensity
  end
  on tick
    self.light.intensity = base * (1 + 0.025 * sin(time * 7.3) + 0.015 * noise(time * 3.1))
  end
end"""


def run_dcc(pix):
    with open(os.path.join(HERE, "chancellor_dcc.py")) as f:
        script = f.read()
    job = {}
    blob = json.dumps(job, sort_keys=True)
    digest = hashlib.sha256((script + blob).encode()).hexdigest()[:16]
    stamp = os.path.join(pix.studio.project, OFFICE, ".job")
    if not (os.path.exists(stamp) and open(stamp).read().strip() == digest):
        import shutil
        shutil.rmtree(os.path.join(pix.studio.project, OFFICE), ignore_errors=True)
        pix.call("dcc_run_script", script=script, args=[blob], name="office", out_dir=OFFICE, timeout_s=1800,
                 description="The Chancellor's Desk furniture and dressing (procedural, Blender)", tags=["chancellors_desk"])
        with open(stamp, "w") as f:
            f.write(digest)
    names = ["desk", "banker_lamp", "flag_pole_in", "flag_cloth_in", "pen", "inkwell", "pedestal", "sconce", "curtain"]
    return {n: pix.call("asset_import", path=f"{OFFICE}/{n}.glb", normalize=False) for n in names}


def paint(project):
    if not os.path.exists(os.path.join(project, ART, "portraits", "vass.png")) or os.environ.get("CD_REPAINT"):
        subprocess.check_call([sys.executable, os.path.join(HERE, "chancellor_art.py"), os.path.join(project, ART)])


def mat_files(project, model):
    """The material assets the importer made for a model (name -> path)."""
    out = {}
    for f in os.listdir(os.path.join(project, OFFICE)):
        if f.startswith(model + "_") and f.endswith(".mat.json"):
            out[f[len(model) + 1:-9]] = f"{OFFICE}/{f}"
        elif f == model + ".mat.json":
            out[""] = f"{OFFICE}/{f}"
    return out


# ============================================================================================
# The room
# ============================================================================================
def materials(pix, project):
    M = {}
    M["parquet"] = ph.texture(pix, "herringbone_parquet", tiling=1.2, triplanar=True, roughness=0.55)
    M["panel"] = ph.texture(pix, "dark_paneled_wood", tiling=0.9, triplanar=True, roughness=0.6)
    M["walnut"] = ph.texture(pix, "american_walnut_veneer", tiling=1.4, triplanar=True, roughness=0.5)
    M["leather"] = ph.texture(pix, "brown_leather", tiling=2.0, triplanar=True, color="#3a5a40", roughness=0.75)
    M["marble"] = ph.texture(pix, "marble_01", tiling=1.0, triplanar=True, roughness=0.6)
    M["damask"] = ph.texture(pix, "beige_wall_001", tiling=1.0, triplanar=True, color="#3f5c4c", roughness=0.9)
    plaster = pix.call("texture_generate", kind="noise", name="textures/plaster", color1="#cfc4ae", color2="#bdb197", scale=2.0,
                       variation=0.2)
    M["plaster"] = plaster["material"]
    pix.material("materials/brass.mat.json", preset="gold", color="#c7a060", roughness=0.32, metallic=1.0)
    M["brass"] = "materials/brass.mat.json"
    pix.material("materials/bronze_frame.mat.json", color="#2c2620", roughness=0.4, metallic=0.85)
    M["frame"] = "materials/bronze_frame.mat.json"
    pix.material("materials/glass_green.mat.json", color="#1f6a3cd8", roughness=0.08, metallic=0.0, emissive=[0.25, 0.9, 0.4, 0.35],
                 doubleSided=True)
    pix.material("materials/shade_inner.mat.json", color="#fff0d8", roughness=0.6, emissive=[1.0, 0.82, 0.55, 6.0], doubleSided=True)
    pix.material("materials/velvet.mat.json", preset="velvet", color="#5e1418", roughness=0.85, doubleSided=True)
    pix.material("materials/window_glass.mat.json", color="#a8bccc22", roughness=0.04, metallic=0.0, doubleSided=True)
    pix.material("materials/rain_glass.mat.json", color="#d6e2ee90", texture=f"{ART}/rain_glass.png", unlit=True, doubleSided=True)
    pix.material("materials/city.mat.json", color="#ffffff", texture=f"{ART}/city_dusk.jpg", unlit=True, emissive=[1, 1, 1, 0.0])
    pix.material("materials/rug.mat.json", color="#ffffff", texture=f"{ART}/rug.jpg", roughness=0.95)
    for k, tex in (("republic", "flag_republic.png"), ("standard", "flag_standard.png")):
        pix.material(f"materials/flag_{k}.mat.json", color="#ffffff", texture=f"{ART}/{tex}", roughness=0.8, doubleSided=True)
    for k, tex in (("decree", "decree_page.jpg"), ("newspaper", "newspaper_page.jpg"), ("letter", "letter_0.jpg")):
        pix.material(f"materials/paper_{k}.mat.json", color="#d8d0c0", texture=f"{ART}/{tex}", roughness=0.92, doubleSided=True)
    pix.material("materials/ceiling.mat.json", color="#3a342c", roughness=0.9)
    return M


def room(cir, M):
    b = cir
    T = 0.3
    # floor, ceiling and walls (thick boxes so the sun only enters through the window)
    b.e("Floor", "cube", pos=(0, -0.05, (RZ0 + RZ1) / 2), scale=(2 * RW + 1, 0.1, RZ1 - RZ0 + 1), material=M["parquet"])
    b.e("Ceiling", "cube", pos=(0, RH + 0.15, (RZ0 + RZ1) / 2), scale=(2 * RW + 1, 0.3, RZ1 - RZ0 + 1), material=M["plaster"])
    zc = RZ0 - T / 2
    b.e("Wall Window L", "cube", pos=(-(RW + WIN_X) / 2 - 0.25, RH / 2, zc), scale=(RW - WIN_X + 0.5, RH, T), material=M["damask"])
    b.e("Wall Window R", "cube", pos=((RW + WIN_X) / 2 + 0.25, RH / 2, zc), scale=(RW - WIN_X + 0.5, RH, T), material=M["damask"])
    b.e("Wall Window Top", "cube", pos=(0, (WIN_Y1 + RH) / 2 + 0.1, zc), scale=(2 * WIN_X + 0.2, RH - WIN_Y1 + 0.2, T),
        material=M["damask"])
    b.e("Wall Window Sill", "cube", pos=(0, WIN_Y0 / 2, zc), scale=(2 * WIN_X + 0.2, WIN_Y0, T), material=M["panel"])
    b.e("Wall Left", "cube", pos=(-RW - T / 2, RH / 2, (RZ0 + RZ1) / 2), scale=(T, RH, RZ1 - RZ0 + 0.6), material=M["damask"])
    b.e("Wall Right", "cube", pos=(RW + T / 2, RH / 2, (RZ0 + RZ1) / 2), scale=(T, RH, RZ1 - RZ0 + 0.6), material=M["damask"])
    b.e("Wall Door", "cube", pos=(0, RH / 2, RZ1 + T / 2), scale=(2 * RW + 0.6, RH, T), material=M["damask"])
    # wainscot, chair rail, skirting and crown moulding on every wall
    for name, pos, scale in (("L", (-RW + 0.025, 0.63, (RZ0 + RZ1) / 2), (0.05, 1.26, RZ1 - RZ0)),
                             ("R", (RW - 0.025, 0.63, (RZ0 + RZ1) / 2), (0.05, 1.26, RZ1 - RZ0)),
                             ("D", (0, 0.63, RZ1 - 0.025), (2 * RW, 1.26, 0.05)),
                             ("WL", (-(RW + WIN_X) / 2 - 0.15, 0.63, RZ0 + 0.025), (RW - WIN_X - 0.3, 1.26, 0.05)),
                             ("WR", ((RW + WIN_X) / 2 + 0.15, 0.63, RZ0 + 0.025), (RW - WIN_X - 0.3, 1.26, 0.05))):
        b.e(f"Wainscot {name}", "cube", pos=pos, scale=scale, material=M["panel"])
        rail = (pos[0], 1.29, pos[2])
        b.e(f"Chair Rail {name}", "cube", pos=rail, scale=(scale[0] + 0.04, 0.06, scale[2] + 0.04), material=M["walnut"])
        b.e(f"Skirting {name}", "cube", pos=(pos[0], 0.09, pos[2]), scale=(scale[0] + 0.03, 0.18, scale[2] + 0.03), material=M["walnut"])
        b.e(f"Crown {name}", "cube", pos=(pos[0], RH - 0.12, pos[2]), scale=(scale[0] + 0.12, 0.24, scale[2] + 0.12),
            material=M["walnut"])
    # the window: reveal, pilasters, bronze frame and mullions, glass and rain, a deep sill
    for s in (-1, 1):
        b.e(f"Window Reveal {s}", "cube", pos=(s * (WIN_X + 0.05), (WIN_Y0 + WIN_Y1) / 2, zc), scale=(0.1, WIN_Y1 - WIN_Y0, T + 0.02),
            material=M["walnut"])
        b.e(f"Pilaster {s}", "cube", pos=(s * (WIN_X + 0.28), RH / 2, RZ0 + 0.06), scale=(0.36, RH, 0.12), material=M["walnut"])
        b.e(f"Pilaster Base {s}", "cube", pos=(s * (WIN_X + 0.28), 0.2, RZ0 + 0.09), scale=(0.42, 0.4, 0.18), material=M["walnut"])
        b.e(f"Pilaster Cap {s}", "cube", pos=(s * (WIN_X + 0.28), RH - 0.36, RZ0 + 0.09), scale=(0.44, 0.14, 0.18), material=M["walnut"])
    b.e("Window Head", "cube", pos=(0, WIN_Y1 + 0.06, zc), scale=(2 * WIN_X + 0.2, 0.12, T + 0.02), material=M["walnut"])
    b.e("Window Sill", "cube", pos=(0, WIN_Y0 - 0.02, RZ0 + 0.06), scale=(2 * WIN_X + 0.5, 0.06, 0.42), material=M["walnut"])
    gz = RZ0 - 0.2
    for x in (-WIN_X + 0.03, -0.47, 0.47, WIN_X - 0.03):
        b.e(f"Mullion {x}", "cube", pos=(x, (WIN_Y0 + WIN_Y1) / 2, gz), scale=(0.06, WIN_Y1 - WIN_Y0, 0.08), material=M["frame"])
    for y in (WIN_Y0 + 0.03, 2.75, WIN_Y1 - 0.03):
        b.e(f"Transom {y}", "cube", pos=(0, y, gz), scale=(2 * WIN_X, 0.06, 0.08), material=M["frame"])
    for k, y in enumerate((1.4, 2.1)):
        b.e(f"Glazing Bar {k}", "cube", pos=(0, y, gz), scale=(2 * WIN_X, 0.025, 0.04), material=M["frame"])
    b.e("Window Glass", "quad", pos=(0, (WIN_Y0 + WIN_Y1) / 2, gz - 0.02), scale=(2 * WIN_X, WIN_Y1 - WIN_Y0, 1),
        material="materials/window_glass.mat.json", castShadows=False)
    b.e("Rain On Glass", "quad", pos=(0, (WIN_Y0 + WIN_Y1) / 2, gz - 0.03), scale=(2 * WIN_X, WIN_Y1 - WIN_Y0, 1),
        material="materials/rain_glass.mat.json", castShadows=False)
    # the city beyond: a lit backdrop (the horizon at eye level), and rain falling between
    b.e("City At Dusk", "quad", pos=(0, 6.5, -20), scale=(46, 23, 1), material="materials/city.mat.json", castShadows=False)
    b.e("Rug", "quad", pos=(0, 0.006, -1.25), rot=(-90, 0, 0), scale=(4.6, 3.2, 1), material="materials/rug.mat.json",
        castShadows=False)
    b.flush("The office: walls, wainscot, window, rug")


def furnish(cir, pix, aur, M, models, project):
    b = cir
    dx, dz = DESK
    # Blender pieces: desk, lamp, flags, curtains, desk set, pedestal, sconces
    ph.place(b, models["desk"], "Desk", (dx, 0, dz))
    ph.place(b, models["banker_lamp"], "Banker Lamp", (dx + 0.66, TOP, dz - 0.18), yaw=200)
    ph.place(b, models["inkwell"], "Inkwell", (dx + 0.32, TOP, dz - 0.12), yaw=-10)
    ph.place(b, models["pen"], "Fountain Pen", (dx + 0.17, TOP + 0.008, dz + 0.2), yaw=58)
    for s, flag in ((-1, "republic"), (1, "standard")):
        root = f"Flag {flag.title()}"
        b.e(root, pos=(s * 2.2, 0, RZ0 + 0.55), rot=(0, 0 if s < 0 else 180, 0), tags=["flag"])
        ph.place(b, models["flag_pole_in"], f"{root} Pole", (0, 0, 0), parent=root)
        b.op("entity_create", name=f"{root} Cloth", parent=root, position=[0, 0, 0],
             components={"mesh": {"mesh": f"asset:{OFFICE}/flag_cloth_in.glb", "material": f"materials/flag_{flag}.mat.json",
                                  "doubleSided": True, "castShadows": False}})
    for s in (-1, 1):
        b.op("entity_create", name=f"Curtain {s}", position=[s * (WIN_X + 0.55), 0.02, RZ0 + 0.2], rotation=[0, 0, 0],
             components={"mesh": {"mesh": f"asset:{OFFICE}/curtain.glb", "material": "materials/velvet.mat.json", "doubleSided": True}})
    b.e("Curtain Rod", "cylinder", pos=(0, 3.92, RZ0 + 0.22), rot=(0, 0, 90), scale=(0.04, 4.6, 0.04), material=M["brass"])
    ph.place(b, models["pedestal"], "Pedestal", (-3.7, 0, -2.75))
    for k, (x, z, yaw) in enumerate(((-RW + 0.02, -2.2, 90), (-RW + 0.02, 1.4, 90), (RW - 0.02, -2.2, -90), (RW - 0.02, 1.4, -90))):
        ph.place(b, models["sconce"], f"Sconce {k}", (x, 2.25, z), yaw=yaw)
    b.flush("Desk, lamp, flags, curtains, sconces")
    # Poly Haven pieces (CC0)
    P = {k: ph.model(pix, k) for k in ("dining_chair_02", "ArmChair_01", "GothicCabinet_01", "vintage_cabinet_01", "marble_bust_01",
                                       "book_encyclopedia_set_01", "fancy_picture_frame_01", "magnifying_glass_01",
                                       "brass_vase_01")}
    ph.place(b, P["dining_chair_02"], "Chancellor's Chair", (dx + 0.05, 0, dz - 0.82), yaw=4)
    ph.place(b, P["ArmChair_01"], "Armchair Left", (-0.95, 0, -0.15), yaw=163)
    ph.place(b, P["ArmChair_01"], "Armchair Right", (0.95, 0, -0.15), yaw=197)
    ph.place(b, P["GothicCabinet_01"], "Gothic Cabinet", (-RW + 0.32, 0, 0.3), yaw=90)
    ph.place(b, P["vintage_cabinet_01"], "Hutch", (RW - 0.32, 0, 0.6), yaw=-90)
    ph.place(b, P["marble_bust_01"], "Bust", (-3.7, 1.09, -2.75), yaw=40)
    ph.place(b, P["book_encyclopedia_set_01"], "Books", (RW - 0.36, 0.0, -1.6), yaw=-90, scale=0.6)
    ph.place(b, P["fancy_picture_frame_01"], "Painting", (RW - 0.03, 2.35, -1.25), yaw=-90)
    ph.place(b, P["brass_vase_01"], "Vase", (-RW + 0.4, 1.86, 0.3), yaw=0)
    b.flush("Furniture and props (Poly Haven CC0)")
    # papers on the desk: the decree, the Ledger, a stack of memoranda
    for name, mat, pos, size, yaw in (("Decree Page", "decree", (dx + 0.02, TOP + 0.003, dz + 0.16), (0.21, 0.29), 4),
                                      ("Ledger", "newspaper", (dx - 0.45, TOP + 0.004, dz + 0.04), (0.34, 0.46), -14),
                                      ("Memo 1", "letter", (dx + 0.42, TOP + 0.002, dz + 0.22), (0.2, 0.27), 11),
                                      ("Memo 2", "letter", (dx + 0.44, TOP + 0.005, dz + 0.2), (0.2, 0.27), 3)):
        b.e(name, "quad", pos=pos, rot=(-90, yaw, 0), scale=(size[0], size[1], 1), material=f"materials/paper_{mat}.mat.json",
            castShadows=True)
    b.flush("Papers on the desk")
    # swap the Blender materials for photoscans
    desk_m = mat_files(project, "desk")
    for k, path in desk_m.items():
        src = {"walnut": M["walnut"], "leather": M["leather"], "brass": M["brass"]}.get(k)
        if src:
            with open(os.path.join(project, src)) as f:
                fields = {kk: v for kk, v in json.load(f).items() if kk not in ("format", "version")}
            pix.call("material_update", path=path, **fields)
    for model, table in (("banker_lamp", {"brass": M["brass"]}), ("inkwell", {"brass": M["brass"]}), ("pen", {"brass": M["brass"]}),
                         ("flag_pole_in", {"brass": M["brass"]}), ("sconce", {"brass": M["brass"]}),
                         ("pedestal", {"marble": M["marble"]})):
        for k, path in mat_files(project, model).items():
            if k in table:
                with open(os.path.join(project, table[k])) as f:
                    fields = {kk: v for kk, v in json.load(f).items() if kk not in ("format", "version")}
                pix.call("material_update", path=path, **fields)
    for k, path in mat_files(project, "banker_lamp").items():
        if k == "glass_green":
            pix.call("material_update", path=path, color="#1d6236", roughness=0.08, emissive=[0.2, 0.85, 0.35, 0.25], doubleSided=True)
        if k == "shade_inner":
            pix.call("material_update", path=path, color="#fff0d8", emissive=[1.0, 0.8, 0.5, 2.5], doubleSided=True)
    for k, path in mat_files(project, "sconce").items():
        if k == "sconce_glass":
            pix.call("material_update", path=path, color="#ffe8c8", emissive=[1.0, 0.75, 0.45, 6.0], doubleSided=True)


def light(aur):
    dx, dz = DESK
    aur.light("Lamp Light", (dx + 0.6, TOP + 0.33, dz - 0.05), "#ffb46a", 2.8, 3.2)
    aur.light("Lamp Pool", (dx + 0.4, TOP + 0.55, dz + 0.1), "#ffc888", 0.9, 2.2)
    for k, (x, z) in enumerate(((-RW + 0.3, -2.2), (-RW + 0.3, 1.4), (RW - 0.3, -2.2), (RW - 0.3, 1.4))):
        aur.light(f"Sconce Light {k}", (x, 2.35, z), "#ffc38a", 1.8, 3.8)
    aur.light("Window Spill", (0, 2.0, RZ0 + 0.6), "#7d9ccf", 1.2, 4.5)
    aur.light("Door Fill", (0, 2.6, 3.2), "#ffcf9c", 0.6, 6.0)
    aur.flush("Lamplight")
    aur.call("fx_create", effect="rain", name="Rain Outside", position=[0, 9, -11],
             overrides={"shapeSize": [16, 1, 14], "rate": 700, "sizeStart": 0.008, "sizeEnd": 0.008, "floorHeight": -12, "collide": False, "intensity": 1.6})
    aur.call("environment_update", skyMode="gradient", skyTop="#0d1424", skyHorizon="#2a3448", ground="#1a1612", ambient=0.42,
             sunAzimuth=188, sunElevation=31, sunColor="#9cb6e6", sunIntensity=2.0, godRays=1.0, haze=0.018, fogDensity=0.0, shadowDistance=14,
             tonemap="agx", exposure=1.4, autoExposure=False, bloomIntensity=0.45, bloomThreshold=1.2, vignette=0.42, grain=0.05,
             ao=1.0, aoRadius=0.6, gi=1.0, giDistance=4.0, ssr=1.0, showGrid=False, saturation=1.02, contrast=1.08,
             shadowSoftness=0.4, windSpeed=3.0, windDirection=20)


# ============================================================================================
# UI
# ============================================================================================
def stat(name, label, value, bar_style=""):
    return {"type": "panel", "name": f"{name} Block", "layout": "column", "gap": 3, "fit": "both", "children": [
        {"type": "text", "name": f"{name} Label", "text": label, "style": "cap", "fit": "both"},
        {"type": "text", "name": name, "text": value, "style": "stat", "fit": "both"},
        {"type": "progress", "name": f"{name} Bar", "value": 0.5, "size": [150, 5], "style": bar_style}]}


def ui(stra, project):
    os.makedirs(os.path.join(project, "ui"), exist_ok=True)
    with open(os.path.join(project, "ui", "chancellery.uistyle.json"), "w") as f:
        json.dump(UI_STYLE, f, indent=2)
    news_cols = ("The fate of the seven bridges, and perhaps of the Provisional Charter itself, now rests with one signature. After "
                 "nine hours of debate the Assembly passed the Reconstruction Bill by fourteen votes.",
                 "Members from the eastern provinces cheered from the gallery; the Army benches sat in silence. General Vass told "
                 "reporters the Engineer Corps would not serve under civilian clerks.",
                 "The Treasury has not denied that the Bond may fail to sell. Minister Ardanne was seen leaving the Chancellery at "
                 "midnight. Continued on page three.")
    stra.call("ui_create", canvas={"name": "Office UI", "theme": "dark", "styleSheet": "ui/chancellery.uistyle.json", "sortOrder": 10},
              elements=[
        {"type": "panel", "name": "Dateline", "anchor": "top_left", "position": [36, 30], "style": "hud", "layout": "row", "gap": 14,
         "align": "center", "fit": "both", "children": [
             {"type": "image", "name": "Seal", "image": f"{ART}/seal.png", "size": [46, 46]},
             {"type": "panel", "name": "Dateline Text", "layout": "column", "gap": 2, "fit": "both", "children": [
                 {"type": "text", "name": "Office", "text": "The Chancellery · Mirovan", "style": "cap", "fit": "both"},
                 {"type": "text", "name": "Date", "text": "Monday, 4 November 1946 · 11:40 p.m.", "fit": "both",
                  "css": {"fontSize": 21, "color": "#f0e6d0"}}]}]},
        {"type": "panel", "name": "Stats", "anchor": "top_right", "position": [-36, 30], "style": "hud", "layout": "row", "gap": 26,
         "fit": "both", "children": [stat("Approval", "Approval", "52%"), stat("Stability", "Stability", "61%", "bar_blue"),
                                     stat("Treasury", "Treasury", "Kr 1.24bn", "bar_red")]},
        # the newspaper
        {"type": "panel", "name": "Newspaper", "anchor": "center", "position": [-250, 10], "size": [720, 0], "fit": "height",
         "style": "newsprint", "layout": "column", "gap": 8, "padding": [26, 34], "visible": False, "children": [
             {"type": "text", "name": "Masthead", "text": "The Mirovan Ledger", "style": "masthead", "size": [652, 0], "fit": "height"},
             {"type": "panel", "name": "Rule 1", "size": [652, 2], "css": {"background": "#1c1a18"}},
             {"type": "panel", "name": "Dateline Row", "layout": "row", "justify": "space_between", "size": [652, 18], "children": [
                 {"type": "text", "name": "Vol", "text": "Vol. LXXII · No. 18,204", "style": "dateline", "fit": "both"},
                 {"type": "text", "name": "Paper Date", "text": "Monday, November 4, 1946", "style": "dateline", "fit": "both"},
                 {"type": "text", "name": "Price", "text": "Two Crowns", "style": "dateline", "fit": "both"}]},
             {"type": "panel", "name": "Rule 2", "size": [652, 1], "css": {"background": "#1c1a18"}},
             {"type": "text", "name": "Headline", "text": "ASSEMBLY SENDS BRIDGES DECREE TO THE CHANCELLOR", "style": "headline",
              "size": [652, 0], "fit": "height"},
             {"type": "text", "name": "Deck", "text": "Vote of 212 to 198 · Army demands command of the works · Treasury warns of the Bond",
              "style": "deck", "size": [652, 0], "fit": "height"},
             {"type": "image", "name": "Press Photo", "image": f"{ART}/news_photo.jpg", "size": [652, 270]},
             {"type": "text", "name": "Photo Caption", "text": "Crowds wait in the rain on the Assembly steps for the result of the vote.",
              "style": "caption", "size": [652, 0], "fit": "height"},
             {"type": "panel", "name": "Columns", "layout": "row", "gap": 18, "size": [652, 0], "fit": "height", "align": "start",
              "children": [{"type": "text", "name": f"Column {i + 1}", "text": t, "style": "column", "size": [205, 0], "fit": "height"}
                           for i, t in enumerate(news_cols)]}]},
    ])
    # the decree: the `document` template, restyled as parchment with the seal
    stra.call("ui_create", template="document", parent="Office UI")
    body = ("By authority of the Provisional Charter, and with the consent of the Assembly, the Chancellor decrees that the "
            "seven bridges of the River Vey destroyed in the late war shall be rebuilt within two years.\n\n"
            "<b>I.</b> The Treasury shall raise nine hundred million crowns by the Reconstruction Bond, offered at four percent.\n"
            "<b>II.</b> The Army Engineer Corps shall serve under civil authority for the duration of the works.\n"
            "<b>III.</b> Every province east of the Vey shall receive a share of the contracts in proportion to its losses.\n\n"
            "<i>Given at Mirovan, the fourth of November, 1946.</i>")
    for name, comp in (
            ("Document", {"position": [250, 0], "size": [640, 900], "style": "sheet", "padding": [34, 46, 30, 46], "visible": False}),
            ("Document Heading", {"text": "Republic of Kestria · Office of the Chancellor", "style": "doc_head"}),
            ("Document Title", {"text": "Decree No. 14", "style": "doc_title"}),
            ("Document Body", {"text": body, "style": "doc_body", "size": [548, 0]}),
            ("Reject", {"text": "Veto", "style": "veto"}),
            ("Sign", {"text": "Sign", "style": "sign"})):
        stra.call("entity_update", entity=name, components={"ui": comp})
    stra.call("entity_update", entity="Reject", name="Veto")
    # the stamp lands on the decree; the pointer is drawn above everything (created last)
    stra.call("ui_create", parent="Office UI", elements=[
        {"type": "image", "name": "Stamp", "image": f"{ART}/stamp_signed.png", "anchor": "center", "position": [300, 210],
         "size": [330, 190], "visible": False, "interactable": False},
        {"type": "image", "name": "Cursor", "image": "ui/cursor.png", "anchor": "top_left", "position": [1500, 760], "size": [28, 36],
         "visible": False, "interactable": False}])
    # the advisors' dialogue box
    stra.call("ui_create", template="dialogue", canvas={"name": "Dialogue UI", "theme": "dark", "styleSheet": "ui/chancellery.uistyle.json",
                                                        "sortOrder": 20})
    stra.call("entity_update", entity="Dialogue Box", components={"ui": {"visible": False, "size": [0, 262], "margin": [0, 230, 40, 230],
                                                                         "padding": [18, 28, 18, 18], "gap": 26}})
    stra.call("entity_update", entity="Dialogue Portrait", components={"ui": {"size": [182, 224]}})
    stra.call("entity_update", entity="Dialogue Choices", components={"ui": {"visible": False, "position": [-250, -330], "size": [660, 0]}})


def cursor_art(project):
    from PIL import Image, ImageDraw
    os.makedirs(os.path.join(project, "ui"), exist_ok=True)
    im = Image.new("RGBA", (120, 152), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pts = [(8, 6), (8, 118), (36, 92), (56, 140), (78, 130), (58, 84), (98, 84)]
    d.polygon([(x + 4, y + 6) for x, y in pts], fill=(0, 0, 0, 110))
    d.polygon(pts, fill=(250, 244, 230, 255))
    d.line(pts + [pts[0]], fill=(20, 16, 12, 255), width=5)
    im.save(os.path.join(project, "ui", "cursor.png"))


# ============================================================================================
# Build
# ============================================================================================
def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    project = studio.project
    paint(project)
    cursor_art(project)
    os.makedirs(os.path.join(project, "story"), exist_ok=True)
    with open(os.path.join(project, "story", "briefing.dialogue"), "w") as f:
        f.write(DIALOGUE)
    nim.call("scene_new", name="The Chancellor's Desk", empty=True)
    models = run_dcc(pix)
    M = materials(pix, project)
    room(cir, M)
    furnish(cir, pix, aur, M, models, project)
    light(aur)
    nim.call("dialogue_check", path="story/briefing.dialogue")
    stra.op("entity_create", name="Office Camera", position=[0.0, 1.45, 3.6],
            components={"camera": {"fov": 40, "nearPlane": 0.05, "farPlane": 300, "primary": True, "aperture": 2.8, "focusDistance": 5.4}})
    stra.op("entity_create", name="Advisors", position=[0, 0, 0],
            components={"dialogue": {"script": "story/briefing.dialogue", "startNode": "Start", "typewriter": 46,
                                     "portraits": f"{ART}/portraits"}})
    stra.op("entity_create", name="Chancellery", position=[0, 0, 0])
    stra.flush("Camera, advisors, chancellery")
    ui(stra, project)
    stra.call("transform", entity="Office Camera", position=[0.0, 1.45, 3.6])
    stra.call("camera_set", eye=[0.0, 1.45, 3.6], target=[0, 1.15, -2.4])
    stra.behave("Chancellery", "Chancellery", "Briefing, documents, decision, consequences.", CAMPAIGN)
    stra.behave("Lamp Light", "Lamplight", "Breathe like an old filament.", LAMP)
    stra.flush("Behaviors")
    look_at(stra, "Office Camera", [0.0, 1.45, 3.6], [0, 1.15, -2.4])
    add_shots(nim)
    nim.call("scene_save", path="scenes/main.sky.json")


def look_at(b, name, eye, target):
    import math
    dx, dy, dz = (target[i] - eye[i] for i in range(3))
    yaw = math.degrees(math.atan2(-dx, -dz))
    pitch = math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    b.call("transform", entity=name, rotation=[round(pitch, 3), round(yaw, 3), 0])


# ============================================================================================
# Film: hero shots. Each is a sequence asset (cinematics/<name>.sequence.json) started with the
# simulation: a camera path with a real lens (depth of field), plus events that drive the night
# (briefing, newspaper, decree, signature) and a pointer for the UI interactions.
# ============================================================================================
SHOTS = [
    dict(name="the_office", dur=8.0, still=4.0, hud=False, fov=40, aperture=2.8, focus=5.6, exposure=1.75,
         points=[(-0.35, 1.52, 4.05), (-0.15, 1.47, 3.55), (0.05, 1.43, 3.1)], target=(0.0, 1.2, -2.4), events=[]),
    dict(name="across_the_desk", dur=8.0, still=5.5, hud=False, fov=36, aperture=2.0, focus=0.85,
         points=[(-1.45, 1.22, -0.95), (-0.85, 1.12, -1.18), (-0.2, 1.06, -1.3)], target=(0.02, 0.79, -1.74), events=[]),
    dict(name="rain_on_the_glass", dur=7.0, still=3.5, hud=False, fov=38, aperture=2.2, focus=2.3,
         points=[(2.35, 1.55, -1.2), (2.15, 1.6, -1.45), (1.95, 1.65, -1.7)], target=(-0.2, 2.0, -4.2), events=[]),
    dict(name="lamplight", dur=7.0, still=3.5, hud=False, fov=32, aperture=1.8, focus=0.72,
         points=[(1.05, 0.98, -1.35), (0.92, 0.95, -1.45), (0.78, 0.93, -1.55)], target=(0.45, 0.84, -2.02), events=[]),
    dict(name="the_briefing", dur=7.0, still=2.9, hud=True, fov=40, aperture=2.8, focus=4.2,
         points=[(1.7, 1.55, 1.9), (1.5, 1.53, 1.6), (1.3, 1.52, 1.3)], target=(-0.1, 1.25, -2.5),
         events=[(0.1, "briefing")]),
    dict(name="the_ledger", dur=7.0, still=3.6, hud=True, fov=40, aperture=2.8, focus=4.5,
         points=[(-1.4, 1.6, 2.2), (-1.2, 1.58, 1.9), (-1.0, 1.56, 1.6)], target=(0.3, 1.2, -2.4),
         events=[(0.0, "no_autoplay"), (0.3, "paper_show")]),
    dict(name="the_general", dur=8.0, still=4.6, hud=True, fov=38, aperture=2.4, focus=4.0,
         points=[(0.4, 1.4, 2.4), (0.2, 1.4, 2.1), (0.0, 1.4, 1.8)], target=(0.0, 1.3, -2.4),
         events=[(0.1, "briefing_general")]),
    dict(name="the_decree", dur=7.0, still=4.8, hud=True, fov=40, aperture=2.8, focus=4.4,
         points=[(-0.5, 1.5, 2.6), (-0.4, 1.48, 2.35), (-0.3, 1.46, 2.1)], target=(0.0, 1.15, -2.2),
         events=[(0.0, "no_autoplay"), (0.2, "decree_show"), (3.5, "sign")],
         cursor=[(0.0, (1620, 1010)), (2.8, (1452, 942)), (3.3, (1450, 944)), (7.0, (1456, 948))]),
]


def add_shots(nim):
    for sh in SHOTS:
        path = f"cinematics/{sh['name']}.sequence.json"
        cam = f"Cam {sh['name']}"
        seq = f"Shot {sh['name']}"
        nim.call("sequence_create", path=path, duration=sh["dur"], entity=seq, play_on_start=False, overwrite=True)
        nim.call("sequence_camera_shot", sequence=path, camera=cam, shot="path", start=0, duration=sh["dur"],
                 points=[list(p) for p in sh["points"]], target=list(sh["target"]), fov=sh["fov"], ease="smooth")
        lens = {"fov": sh["fov"], "primary": False, "nearPlane": 0.03, "farPlane": 300}
        if sh.get("aperture"):
            lens.update(aperture=sh["aperture"], focusDistance=sh["focus"])
        nim.call("entity_update", entity=cam, components={"camera": lens})
        # one event per key time (a key at an existing time replaces it)
        events, used = [], set()
        for t, e in [(0.0, "hud_show" if sh["hud"] else "hud_hide")] + list(sh["events"]):
            while round(t, 3) in used:
                t += 0.005
            used.add(round(t, 3))
            events.append({"t": round(t, 3), "event": e, "target": "Chancellery"})
        keys = []
        if sh.get("cursor"):
            keys.append({"entity": "Cursor", "property": "ui.visible", "t": 0.0, "value": True, "ease": "step"})
            for t, (x, y) in sh["cursor"]:
                keys.append({"entity": "Cursor", "property": "ui.position", "t": t, "value": [x, y], "ease": "smooth"})
        if sh.get("exposure"):
            keys.append({"property": "environment.exposure", "t": 0.0, "value": sh["exposure"], "ease": "step"})
        nim.call("sequence_key", sequence=path, events=events, keys=keys)


def shots():
    """showcase.py render hooks: start each shot's sequence with the simulation, then film it."""
    out = []
    for sh in SHOTS:
        seq = f"cinematics/{sh['name']}.sequence.json"

        def start(sky, seq=seq):
            sky.call("sequence_play", sequence=seq)
        out.append(dict(name=sh["name"], frames=int(sh["dur"] * 30), warmup=0, view="scene", start=start, still=sh["still"]))
    return out


def render_stills(project, out_dir="shots", width=1920, height=1080, samples=16, only=None):
    """Final stills: each shot's sequence is played from the start and captured at its `still` time (JPEG q90)."""
    from sky import Sky
    os.makedirs(os.path.join(project, out_dir), exist_ok=True)
    for sh in SHOTS:
        if only and sh["name"] not in only:
            continue
        sky = Sky(project=project, scene="scenes/main.sky.json")
        sky.call("sim_control", action="play")
        sky.call("sequence_play", sequence=f"cinematics/{sh['name']}.sequence.json")
        sky.call("sim_control", action="step", ticks=max(1, int(round(sh["still"] * 60))))
        png = os.path.join(project, out_dir, sh["name"] + ".png")
        sky.call("viewport_capture", view="scene", width=width, height=height, samples=samples, annotate=False, overlays=False,
                 include_image=False, save_path=f"{out_dir}/{sh['name']}.png")
        sky.close()
        jpg = png[:-4] + ".jpg"
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", png, "--out", jpg], check=True,
                       capture_output=True)
        os.remove(png)
        print(sh["name"], os.path.getsize(jpg) // 1024, "KB", flush=True)


def contact_sheet(project, out_dir="shots", cols=2, cell=(960, 540), gutter=8):
    from PIL import Image
    names = [sh["name"] for sh in SHOTS if os.path.exists(os.path.join(project, out_dir, sh["name"] + ".jpg"))]
    rows = (len(names) + cols - 1) // cols
    W, H = cols * cell[0] + (cols + 1) * gutter, rows * cell[1] + (rows + 1) * gutter
    sheet = Image.new("RGB", (W, H), (18, 18, 22))
    for i, n in enumerate(names):
        im = Image.open(os.path.join(project, out_dir, n + ".jpg")).convert("RGB").resize(cell, Image.LANCZOS)
        sheet.paste(im, (gutter + (i % cols) * (cell[0] + gutter), gutter + (i // cols) * (cell[1] + gutter)))
    path = os.path.join(project, out_dir, "contact_sheet.jpg")
    sheet.save(path, quality=88)
    print("contact sheet", os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    proj = os.path.join(root, "examples", "chancellors_desk")
    if len(sys.argv) > 1 and sys.argv[1] == "stills":
        render_stills(proj, only=sys.argv[2:] or None)
        contact_sheet(proj)
    elif len(sys.argv) > 1 and sys.argv[1] == "sheet":
        contact_sheet(proj)

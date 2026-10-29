#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds src/lm_table.h: a character bigram for the character decoder (CVD), estimated from a synthetic
corpus of amateur-radio CW traffic (QSO and contest exchanges built from templates with random calls,
names, places, reports and station details) mixed with plain English.

  tools/lmgen.py [--seed N] [--lines N] [--print-corpus]

Symbols are the CHR table outputs (A-Z, 0-9, / . , ? =) plus the context symbol SPACE (start of a word).
Prosigns are written as letters, as most senders key them. The table holds log P(symbol | context) with
add-0.5 smoothing, interpolated with the corpus unigram."""
import math
import os
import random
import sys

SYMS = [chr(c) for c in range(ord("A"), ord("Z") + 1)] + [str(d) for d in range(10)] + list("/.,?=")
SPACE = len(SYMS)

PREFIX = ("K W N AA AB AC AD AE AF AG AI AJ AK KA KB KC KD KE KF KG KI KJ KK WA WB WD VE VA G M 2E GM GW MM "
          "DL DJ DK DF DG DH DM DO F ON PA PD OZ SM LA OH ES YL LY SP SQ OK OL OM HA YO LZ S5 9A E7 YU I IK IZ "
          "IW EA EB EC CT HB9 OE UA RA RN RU R UR UT EW 4X JA JH JR JE JF BG BY BV HL DS VK ZL ZS PY LU CE XE "
          "KP4 KH6 VP2 8P ZF").split()
NAMES = ("BOB MIKE JIM TOM DAVE BILL RON DON JACK PAUL MARK STEVE GARY PETE FRED KEN ED AL JOE RICK "
         "HANS KLAUS PETER WOLF JUERGEN UWE DIETER JAN PIET HENK LARS OLE MATS PIERRE JEAN LUIGI MARCO JOSE "
         "PACO IVAN SERGEY OLEG IGOR TAKA HIRO KEN YUKI MARIA ANNA SUE MARY LIZ CHRIS ALEX NICK TONY DAN "
         "WALT ART HAL GUS BEN SAM").split()
CITIES = ("MUNICH HAMBURG KOELN WIEN ZURICH PARIS LYON ROMA MILANO MADRID LISBOA LONDON LEEDS DUBLIN "
          "OSLO STOCKHOLM HELSINKI TALLINN RIGA WARSZAWA PRAHA BRNO BUDAPEST SOFIA MOSCOW KIEV MINSK TOKYO "
          "OSAKA SYDNEY PERTH AUCKLAND BOSTON DENVER DALLAS CHICAGO SEATTLE MIAMI ATLANTA TORONTO OTTAWA "
          "MONTREAL OHIO TEXAS MAINE IOWA UTAH IDAHO NR NEAR CITY TOWN VILLAGE").split()
RIGS = ("IC7300 IC7610 IC705 IC718 FT991 FT891 FT817 FT818 FTDX10 FT710 K3 K4 KX2 KX3 TS590 TS890 TS480 "
        "QCX QRPLABS ELECRAFT HOMEBREW KENWOOD YAESU ICOM").split()
ANTS = ("DIPOLE G5RV EFHW VERT VERTICAL YAGI BEAM LOOP WIRE INV V INVV WINDOM HEXBEAM QUAD GP DELTA LOOP "
        "LONG WIRE ENDFED").split()
WX = "SUNNY CLOUDY RAIN SNOW WINDY FOGGY CLDY HOT COLD WARM NICE FB FINE OVC CLR".split()
ENGLISH = """THE OLD MAN AND HIS DOG WENT HOME TO SEE A GOOD FRIEND WHO LIVES NEAR THE RIVER
WE HAVE A NICE DAY HERE WITH SOME RAIN IN THE MORNING BUT NOW THE SUN IS OUT AND IT IS WARM
I AM USING A SMALL RADIO AND A WIRE IN THE TREES ABOUT TEN METERS HIGH IT WORKS VERY WELL
MY WIFE AND KIDS SEND THEIR REGARDS TO YOU AND YOUR FAMILY HOPE ALL IS WELL THERE
I HAVE BEEN A HAM FOR MANY YEARS AND I STILL ENJOY CW MORE THAN ANY OTHER MODE
THE BAND IS OPEN TODAY AND CONDITIONS ARE BETTER THAN LAST WEEK MANY STATIONS FROM EUROPE
IT WAS VERY NICE TO MEET YOU ON THE AIR AND I HOPE TO WORK YOU AGAIN SOON
SORRY I MUST GO NOW FOR DINNER THANK YOU FOR THE CONTACT BEST WISHES AND GOOD LUCK
YOUR SIGNAL IS STRONG BUT THERE IS SOME FADING AND NOISE FROM THE STORM NEAR MY HOUSE
I WORK AS AN ENGINEER AND I AM RETIRED NOW SO I HAVE MORE TIME FOR THE HOBBY
WHAT IS YOUR ANTENNA AND HOW MUCH POWER DO YOU RUN I HEAR YOU VERY WELL TONIGHT
THE WEATHER IS COLD AND WINDY WITH SNOW ON THE GROUND THE TEMPERATURE IS BELOW ZERO
PLEASE SEND YOUR CARD VIA THE BUREAU OR DIRECT I WILL SEND MINE TOMORROW
I LIKE TO OPERATE PORTABLE FROM THE HILLS IN SUMMER WITH A SMALL BATTERY AND A KEY
THERE IS A CONTEST THIS WEEKEND AND THE BAND WILL BE VERY BUSY SO I WILL CALL CQ
""".split()


def call(r):
    c = r.choice(PREFIX)
    if not c[-1].isdigit():
        c += str(r.randint(0, 9))
    c += "".join(r.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(r.choice((1, 2, 2, 3, 3, 3))))
    if r.random() < 0.06:
        c += "/" + r.choice(("P", "M", "QRP", "MM", "1", "2", "3"))
    if r.random() < 0.03:
        c = r.choice(("DL", "F", "EA", "I", "W", "K", "VE3", "G")) + "/" + c
    return c


def rst(r):
    return r.choice(("599", "599", "579", "589", "569", "559", "549", "449", "459", "339", "559", "5NN", "5NN",
                     "4NN", "57N", "59N", "479", "369", "229"))


def num(r):
    return str(r.choice((r.randint(1, 9), r.randint(10, 99), r.randint(100, 999), r.randint(1, 50))))


def qso(r):
    a, b = call(r), call(r)
    n1, n2 = r.choice(NAMES), r.choice(NAMES)
    q1, q2 = r.choice(CITIES), r.choice(CITIES)
    g = r.choice(("GM", "GA", "GE", "GD", "HI", "HELLO", "TNX"))
    om = r.choice(("OM", "DR OM", "DR " + n2, n2, "OM " + n2, "FRIEND", "OP"))
    out = []
    k = r.choice(("K", "K", "PSE K", "KN", "AR", "AR K", "BK"))
    rep = r.choice((1, 2, 3))
    cq = r.choice(("CQ CQ CQ", "CQ CQ", "CQ CQ CQ CQ", "CQ DX", "CQ TEST", "CQ POTA", "CQ SOTA", "CQ FD", "CQ NA"))
    out.append(f"{cq} DE {' '.join([a] * rep)} {k}")
    out.append(f"{a} DE {b} {b} {r.choice(('K', 'AR', 'KN', ''))}")
    s = f"{b} DE {a} {g} {om} TNX FER {r.choice(('CALL', 'THE CALL', 'COMING BACK', 'CALL ES RPRT'))} UR RST {rst(r)}"
    if r.random() < 0.6:
        s += f" {rst(r)}"
    if r.random() < 0.4:
        s += " " + r.choice(("QSB", "QRM", "QRN", "WID QSB", "BUT QSB", "FB", "VY QSB"))
    s += f" NAME {n1} {n1} QTH {q1} {q1}"
    if r.random() < 0.5:
        s += f" {r.choice(('NR', 'NEAR'))} {r.choice(CITIES)}"
    s += f" {r.choice(('HW CPY', 'HW?', 'HW CPY?', 'BK', 'SO HW'))} {b} DE {a} {r.choice(('KN', 'K', 'AR KN'))}"
    out.append(s)
    s = f"{a} DE {b} {r.choice(('R R', 'R', 'FB', 'OK', 'SOLID CPY', 'ALL OK'))} {n1} TNX FER {r.choice(('RPRT', 'NICE RPRT', 'FB RPRT'))}"
    s += f" UR RST {rst(r)} {rst(r)} NAME {n2} QTH {q2}"
    if r.random() < 0.7:
        s += f" RIG {r.choice(RIGS)} PWR {r.choice(('5', '10', '50', '100', '100', '400', '500', '1KW', 'QRP'))}{r.choice(('W', 'W', ''))}"
        s += f" ANT {r.choice(ANTS)}"
        if r.random() < 0.5:
            s += f" UP {r.randint(5, 30)}M"
    if r.random() < 0.6:
        s += f" WX {r.choice(WX)} TEMP {r.randint(-15, 35)}{r.choice(('C', 'C', 'F', ' DEG'))}"
    if r.random() < 0.3:
        s += f" AGE {r.randint(20, 90)} HAM SINCE {r.randint(1955, 2024)}"
    s += f" {r.choice(('HW?', 'BK', 'SO HW CPY?', 'OK?'))} {a} DE {b} {r.choice(('KN', 'K', 'AR'))}"
    out.append(s)
    s = f"{b} DE {a} {r.choice(('R', 'R R', 'FB', 'OK'))} {n2} {r.choice(('TNX FER QSO', 'TNX FER NICE QSO', 'MNI TNX', 'TKS FER FB QSO', 'TNX FER QSO ES RPRT'))}"
    s += f" {r.choice(('HPE CU AGN', 'CUL', 'HPE CUAGN', 'CU AGN SN', 'GL ES GUD DX', 'BEST 73', 'VY 73'))}"
    s += f" {r.choice(('73', '73 ES GL', '73 TU', '73 GB', '73 ES GD DX'))} {b} DE {a} {r.choice(('SK', 'SK TU', 'SK E E', 'TU SK', 'AR SK'))}"
    out.append(s)
    out.append(f"{r.choice(('TU', 'TU 73', '73', 'GL 73', 'R 73'))} {r.choice(('SK', 'E E', 'TU', 'DIT DIT', ''))}")
    return out


def contest(r):
    a = call(r)
    out = [f"{r.choice(('CQ TEST', 'TEST', 'CQ CQ TEST', 'CQ WW', 'CQ SS'))} {a} {r.choice((a, ''))}"]
    for _ in range(r.randint(2, 6)):
        b = call(r)
        x = r.choice(("5NN " + str(r.randint(1, 40)), "5NN " + num(r), "599 " + num(r), "5NN " + r.choice(("14", "15", "16", "25", "28", "3", "4", "5")),
                      "5NN " + r.choice(("K", "W", "PA", "DL", "ON", "MA", "CA", "TX", "NY", "OH")), "TU " + num(r)))
        out.append(f"{b} {x}")
        out.append(f"{r.choice(('TU', 'TU', 'R TU', 'QSL TU', 'TNX'))} {a} {r.choice(('TEST', '', 'UP', 'QRZ?'))}")
        if r.random() < 0.2:
            out.append(r.choice(("AGN?", "NR?", "CL?", "?", "QRZ?", "QRL?", "PSE AGN", "QRS PSE", "QSY UP 1")))
    return out


def english(r):
    w = ENGLISH
    i = r.randrange(len(w))
    return [" ".join(w[i:i + r.randint(5, 25)])]


def misc(r):
    return [r.choice((f"QRL? QRL? DE {call(r)}", f"QRZ? DE {call(r)}", f"VVV VVV DE {call(r)}", f"{call(r)} DE {call(r)} QSL VIA BURO",
                      f"PSE QSL VIA {r.choice(('BURO', 'LOTW', 'EQSL', 'DIRECT', call(r)))}", "QSY 7030", "QRS PSE", "QRM HR SRI",
                      f"UR {r.choice(('SIGS', 'SIG', 'FIST', 'KEYING'))} {r.choice(('FB', 'VY FB', 'GUD', 'STRONG', 'WEAK', 'FADING'))}",
                      f"{r.choice(('GM', 'GA', 'GE'))} {r.choice(NAMES)} {r.choice(('ES', 'AND'))} {r.choice(('ALL', 'FAMILY', 'XYL'))}",
                      f"NR {num(r)} {num(r)}", f"FREQ {r.randint(1800, 29000)} KHZ", "TEST DE " + call(r)))]


def punctuate(r, line):
    # many senders separate phrases with = (BT); some use . and , and ? in plain text
    w = line.split()
    for i in range(len(w) - 1, 0, -1):
        x = r.random()
        if x < 0.06:
            w.insert(i, "=")
        elif x < 0.075:
            w[i - 1] += r.choice((".", ",", "?"))
    return " ".join(w)


def corpus(seed, lines):
    r = random.Random(seed)
    out = []
    while len(out) < lines:
        k = r.random()
        out += qso(r) if k < 0.55 else contest(r) if k < 0.75 else english(r) if k < 0.88 else misc(r)
    return [punctuate(r, x) for x in out]


def main():
    argv = sys.argv[1:]
    seed = int(argv[argv.index("--seed") + 1]) if "--seed" in argv else 1
    lines = int(argv[argv.index("--lines") + 1]) if "--lines" in argv else 60000
    text = corpus(seed, lines)
    if "--print-corpus" in argv:
        print("\n".join(text))
        return
    n = len(SYMS)
    big = [[0.0] * n for _ in range(n + 1)]
    tri = {}
    uni = [0.0] * n
    for line in text:
        p2, prev = SPACE, SPACE
        for ch in " ".join(line.split()):
            if ch == " ":
                p2, prev = prev, SPACE
                continue
            if ch not in SYMS:
                continue
            c = SYMS.index(ch)
            big[prev][c] += 1
            t = tri.setdefault((p2, prev), [0.0] * n)
            t[c] += 1
            uni[c] += 1
            p2, prev = prev, c
    tot = sum(uni)
    puni = [(u + 0.5) / (tot + 0.5 * n) for u in uni]
    rows = []
    for p in range(n + 1):
        rt = sum(big[p])
        lam = rt / (rt + 20.0)                   # trust the bigram once its context has been seen
        rows.append([math.log(lam * (big[p][c] + 0.5) / (rt + 0.5 * n) + (1 - lam) * puni[c]) for c in range(n)])
    # trigram, interpolated with the bigram row of its last context symbol; contexts (a, b) with a, b in
    # symbols + SPACE; rows never seen fall back to the bigram
    trows = []
    for a in range(n + 1):
        for b in range(n + 1):
            t = tri.get((a, b))
            bg = [math.exp(x) for x in rows[b]]
            if not t:
                trows.append(rows[b])
                continue
            tt = sum(t)
            lam = tt / (tt + 10.0)
            trows.append([math.log(lam * (t[c] + 0.1) / (tt + 0.1 * n) + (1 - lam) * bg[c]) for c in range(n)])
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "src", "lm_table.h"), "w") as f:
        f.write("/* SPDX-License-Identifier: GPL-3.0-or-later */\n")
        f.write("/* Generated by tools/lmgen.py: character bigram log P(symbol | context) for CVD, from a synthetic\n"
                " * corpus of CW traffic and plain English. Rows: context (the symbols, then LM_SPACE), columns:\n"
                " * symbols. Do not edit by hand. */\n")
        f.write(f'#define LM_NSYM {n}\n#define LM_SPACE {SPACE}\n')
        f.write('static const char LM_SYMS[LM_NSYM + 1] = "' + "".join(SYMS) + '";\n')
        f.write("static const float LM_UNI[LM_NSYM] = {\n")
        f.write("    " + ", ".join(f"{math.log(x):.3f}f" for x in puni) + "\n};\n")
        f.write("static const float LM_BIGRAM[LM_NSYM + 1][LM_NSYM] = {\n")
        for p, row in enumerate(rows):
            f.write("    { " + ", ".join(f"{x:.3f}f" for x in row) + " },\n")
        f.write("};\n")
        # trigram rows quantised to 1/16 nat in signed 8 bits (log-probabilities down to -8)
        f.write("/* trigram log P(symbol | a, b), row (a * (LM_NSYM + 1) + b), in 1/16 nat, floor -128 */\n")
        f.write("static const signed char LM_TRIGRAM[(LM_NSYM + 1) * (LM_NSYM + 1)][LM_NSYM] = {\n")
        for row in trows:
            f.write("    {" + ",".join(str(max(-128, int(round(x * 16)))) for x in row) + "},\n")
        f.write("};\n")
    print(f"corpus {len(text)} lines, {int(tot)} symbols -> src/lm_table.h")


if __name__ == "__main__":
    main()

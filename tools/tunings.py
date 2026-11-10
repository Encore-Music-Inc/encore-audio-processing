# ENCORE. Audio Processing
# Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
#
# The tuner's instruments, tunings, temperaments and reference pitches, in one
# place. Running this writes the two files made from it:
#   dsp/Tunings.h        for C++ (the desktop tuner)
#   data/tunings.json    for the browser
# CI runs it and fails if either file differs from what it writes.
#
#   python3 tools/tunings.py
#
# Strings are written in the order you see them holding the instrument, low side
# first (so a 5-string banjo's short 5th string comes first). Pitches use
# scientific names (A4 = 440 Hz). Strings played together as a pair or group
# (a course: 12-string, mandolin, lute, bouzouki...) are joined with "+", every
# string written out, e.g. "E2+E3" for an octave pair, "B3+B3" for a unison pair. "transpose" is for horns and reeds: the written
# note is the concert note plus that many semitones.

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Winds and brass: the notes a band (concert Bb) and an orchestra (concert A) tune to,
# then long tones on any note. how: a word for the usual adjustment, or the advice itself
HOW = {
    "headjoint": "Sharp: pull the headjoint out a little. Flat: push it in. Then check a low and a high note too.",
    "barrel": "Sharp: pull the barrel out a little (and the middle joint, if the low notes are sharp too). Flat: push it in.",
    "mouthpiece": "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on.",
    "slide": "Sharp: pull the main tuning slide out. Flat: push it in.",
}
KALIMBA = "Tines left to right as you hold it. Sharp: tap the tine in a little (shorter). Flat: tap it out (longer). Use a small hammer"

def W(bb, a, how):
    how = HOW.get(how, how)
    return [T("Band tuning note (concert Bb)", bb, how), T("Orchestra tuning note (concert A)", a, how),
            T("Any note (long tones)", "", "Hold each note steady: you'll learn which ones run sharp or flat on your instrument")]

def T(name, notes, note=""):
    courses = [c.split("+") for c in notes.split()]
    return {"name": name, "notes": [n for c in courses for n in c], "courses": [len(c) for c in courses], "note": note}

CATALOG = [
    ("Guitars", [
        ("Guitar (6-string)", [
            T("Standard", "E2 A2 D3 G3 B3 E4"),
            T("Half step down (Eb)", "Eb2 Ab2 Db3 Gb3 Bb3 Eb4"),
            T("Whole step down (D)", "D2 G2 C3 F3 A3 D4"),
            T("C standard", "C2 F2 Bb2 Eb3 G3 C4"),
            T("B standard", "B1 E2 A2 D3 F#3 B3"),
            T("Drop D", "D2 A2 D3 G3 B3 E4"),
            T("Drop C#", "C#2 G#2 C#3 F#3 A#3 D#4"),
            T("Drop C", "C2 G2 C3 F3 A3 D4"),
            T("Drop B", "B1 F#2 B2 E3 G#3 C#4"),
            T("Drop A", "A1 E2 A2 D3 F#3 B3"),
            T("Double drop D", "D2 A2 D3 G3 B3 D4"),
            T("DADGAD", "D2 A2 D3 G3 A3 D4"),
            T("Open G", "D2 G2 D3 G3 B3 D4"),
            T("Open D", "D2 A2 D3 F#3 A3 D4"),
            T("Open E", "E2 B2 E3 G#3 B3 E4"),
            T("Open A", "E2 A2 E3 A3 C#4 E4"),
            T("Open C", "C2 G2 C3 G3 C4 E4"),
            T("Open D minor", "D2 A2 D3 F3 A3 D4"),
            T("Open G minor", "D2 G2 D3 G3 Bb3 D4"),
            T("Open C6", "C2 A2 C3 G3 C4 E4"),
            T("All fourths", "E2 A2 D3 G3 C4 F4"),
            T("New standard (Fripp)", "C2 G2 D3 A3 E4 G4"),
            T("Nashville (high strung)", "E3 A3 D4 G4 B3 E4", "The four low strings an octave up, from a 12-string set"),
            T("Open G (Keith Richards 5-string)", "G2 D3 G3 B3 D4"),
        ]),
        ("Guitar (12-string)", [
            T("Standard", "E2+E3 A2+A3 D3+D4 G3+G4 B3+B3 E4+E4", "The four low pairs have an octave string; the B and E pairs are two strings at the same pitch"),
            T("Half step down (Eb)", "Eb2+Eb3 Ab2+Ab3 Db3+Db4 Gb3+Gb4 Bb3+Bb3 Eb4+Eb4"),
            T("Whole step down (D)", "D2+D3 G2+G3 C3+C4 F3+F4 A3+A3 D4+D4"),
            T("Drop D", "D2+D3 A2+A3 D3+D4 G3+G4 B3+B3 E4+E4"),
            T("DADGAD", "D2+D3 A2+A3 D3+D4 G3+G4 A3+A3 D4+D4"),
            T("Open G", "D2+D3 G2+G3 D3+D4 G3+G4 B3+B3 D4+D4"),
            T("Open D", "D2+D3 A2+A3 D3+D4 F#3+F#4 A3+A3 D4+D4"),
        ]),
        ("Guitar (7-string)", [
            T("Standard", "B1 E2 A2 D3 G3 B3 E4"),
            T("Drop A", "A1 E2 A2 D3 G3 B3 E4"),
            T("Half step down", "Bb1 Eb2 Ab2 Db3 Gb3 Bb3 Eb4"),
            T("Russian (7-string)", "D2 G2 B2 D3 G3 B3 D4", "Open G, the Russian guitar's own tuning"),
        ]),
        ("Guitar (8-string)", [
            T("Standard", "F#1 B1 E2 A2 D3 G3 B3 E4"),
            T("Drop E", "E1 B1 E2 A2 D3 G3 B3 E4"),
        ]),
        ("Baritone guitar", [
            T("B standard", "B1 E2 A2 D3 F#3 B3"),
            T("A standard", "A1 D2 G2 C3 E3 A3"),
        ]),
        ("Tenor guitar", [
            T("Standard (fifths)", "C3 G3 D4 A4"),
            T("Chicago", "D3 G3 B3 E4"),
        ]),
        ("Guitalele", [T("Standard", "A2 D3 G3 C4 E4 A4")]),
        ("Requinto guitar", [T("Standard", "A2 D3 G3 C4 E4 A4")]),
        ("Resonator / Dobro", [
            T("Open G (high bass)", "G2 B2 D3 G3 B3 D4"),
            T("Open D", "D2 A2 D3 F#3 A3 D4"),
        ]),
        ("Lap steel", [
            T("C6", "C3 E3 G3 A3 C4 E4"),
            T("A6", "C#3 E3 F#3 A3 C#4 E4"),
            T("Open E", "E2 B2 E3 G#3 B3 E4"),
            T("Open G", "D2 G2 D3 G3 B3 D4"),
        ]),
        ("Pedal steel (10-string)", [
            T("E9", "B2 D3 E3 F#3 G#3 B3 E4 G#4 D#4 F#4", "Strings 10 to 1, open (no pedals)"),
            T("C6", "C2 F2 A2 C3 E3 G3 A3 C4 E4 D4", "Strings 10 to 1, open (no pedals)"),
        ]),
    ]),
    ("Basses", [
        ("Bass (4-string)", [
            T("Standard", "E1 A1 D2 G2"),
            T("Drop D", "D1 A1 D2 G2"),
            T("Half step down", "Eb1 Ab1 Db2 Gb2"),
            T("D standard", "D1 G1 C2 F2"),
            T("Drop C", "C1 G1 C2 F2"),
            T("BEAD", "B0 E1 A1 D2"),
            T("Tenor (ADGC)", "A1 D2 G2 C3"),
            T("Fifths (cello-style)", "C1 G1 D2 A2"),
            T("Piccolo bass", "E2 A2 D3 G3"),
        ]),
        ("Bass (5-string)", [
            T("Standard (low B)", "B0 E1 A1 D2 G2"),
            T("High C", "E1 A1 D2 G2 C3"),
            T("Drop A", "A0 E1 A1 D2 G2"),
        ]),
        ("Bass (6-string)", [T("Standard", "B0 E1 A1 D2 G2 C3")]),
        ("Upright / double bass", [
            T("Orchestral (fourths)", "E1 A1 D2 G2"),
            T("5-string (low B)", "B0 E1 A1 D2 G2"),
            T("Solo tuning", "F#1 B1 E2 A2", "A whole step up, for solo playing with solo strings"),
            T("Fifths", "C1 G1 D2 A2"),
        ]),
        ("Acoustic bass guitar", [T("Standard", "E1 A1 D2 G2")]),
        ("Guitarron (Mexican)", [T("Standard", "A1 D2 G2 C3 E2 A2", "Re-entrant: the 5th and 6th strings drop back down")]),
        ("Bajo sexto", [
            T("Standard", "E1+E2 A1+A2 D2+D3 G2+G2 C3+C3 F3+F3", "About an octave below a 12-string: the three low pairs have an octave string, the top three are pairs at the same pitch"),
            T("Solo (B and E on top)", "E1+E2 A1+A2 D2+D3 G2+G2 B2+B2 E3+E3"),
        ]),
        ("Bajo quinto", [T("Standard", "A1+A2 D2+D3 G2+G2 C3+C3 F3+F3")]),
    ]),
    ("Ukuleles", [
        ("Ukulele (soprano, concert, tenor)", [
            T("Standard (high G)", "G4 C4 E4 A4", "Re-entrant: the G string is higher than the C"),
            T("Low G", "G3 C4 E4 A4"),
            T("D tuning (old-style)", "A4 D4 F#4 B4"),
            T("Slack key (open C)", "G4 C4 E4 G4"),
        ]),
        ("Baritone ukulele", [T("Standard", "D3 G3 B3 E4")]),
        ("Bass ukulele", [T("Standard", "E1 A1 D2 G2")]),
        ("Banjolele", [T("Standard", "G4 C4 E4 A4")]),
    ]),
    ("Mandolins", [
        ("Mandolin", [
            T("Standard", "G3+G3 D4+D4 A4+A4 E5+E5", "Four pairs, both strings of a pair at the same pitch"),
            T("Cross A (AEAE)", "A3+A3 E4+E4 A4+A4 E5+E5"),
            T("Open G (GDGD)", "G3+G3 D4+D4 G4+G4 D5+D5"),
            T("Cross G (GDGB)", "G3+G3 D4+D4 G4+G4 B4+B4"),
        ]),
        ("Mandola", [T("Standard", "C3+C3 G3+G3 D4+D4 A4+A4")]),
        ("Octave mandolin", [
            T("Standard", "G2+G2 D3+D3 A3+A3 E4+E4"),
            T("Octave strings on the low pairs", "G2+G3 D3+D4 A3+A3 E4+E4"),
        ]),
        ("Mandocello", [T("Standard", "C2+C2 G2+G2 D3+D3 A3+A3")]),
    ]),
    ("Banjos", [
        ("Banjo (5-string)", [
            T("Open G", "G4 D3 G3 B3 D4", "The short 5th string comes first"),
            T("Double C", "G4 C3 G3 C4 D4"),
            T("Standard C", "G4 C3 G3 B3 D4"),
            T("Sawmill / modal (gDGCD)", "G4 D3 G3 C4 D4"),
            T("Open G minor (gDGBbD)", "G4 D3 G3 Bb3 D4"),
            T("Open D", "F#4 D3 F#3 A3 D4"),
            T("Open D minor (aDFAD)", "A4 D3 F3 A3 D4"),
            T("Open A", "A4 E3 A3 C#4 E4"),
        ]),
        ("Tenor banjo", [
            T("Standard (fifths)", "C3 G3 D4 A4"),
            T("Irish", "G2 D3 A3 E4"),
        ]),
        ("Plectrum banjo", [T("Standard", "C3 G3 B3 D4")]),
        ("Banjo (6-string)", [T("Standard (guitar)", "E2 A2 D3 G3 B3 E4")]),
    ]),
    ("Orchestral strings", [
        ("Violin / fiddle", [
            T("Standard", "G3 D4 A4 E5"),
            T("Cross A (AEAE)", "A3 E4 A4 E5"),
            T("Cross G (GDGD)", "G3 D4 G4 D5"),
            T("ADAE", "A3 D4 A4 E5"),
            T("Calico (AEAC#)", "A3 E4 A4 C#5"),
            T("DDAD", "D3 D4 A4 D5"),
            T("Baroque (A = 415)", "G3 D4 A4 E5", "Set the reference to 415 Hz"),
        ]),
        ("Viola", [T("Standard", "C3 G3 D4 A4")]),
        ("Cello", [
            T("Standard", "C2 G2 D3 A3"),
            T("Bach suite 5 (scordatura)", "C2 G2 D3 G3"),
        ]),
        ("Hardanger fiddle", [T("Standard (ADAE)", "A3 D4 A4 E5 B3 D4 E4 F#4 A4", "The 4 melody strings, then the 5 sympathetic strings under them (a common setup; it changes with the piece)")]),
        ("Nyckelharpa", [T("Standard", "C3 G3 C4 A4", "The 4 played strings; tune the sympathetic strings by ear")]),
    ]),
    ("Early music", [
        ("Renaissance lute (6-course)", [
            T("In G", "G2+G3 C3+C4 F3+F4 A3+A3 D4+D4 G4", "Single top string; octave strings on the three low pairs (the 4th is sometimes at the same pitch)"),
            T("In A", "A2+A3 D3+D4 G3+G4 B3+B3 E4+E4 A4"),
        ]),
        ("Renaissance lute (8-course)", [T("In G", "D2+D3 F2+F3 G2+G3 C3+C4 F3+F4 A3+A3 D4+D4 G4", "Single top string; octave strings on the five low pairs")]),
        ("Baroque lute (13-course)", [T("D minor", "A1+A2 B1+B2 C2+C3 D2+D3 E2+E3 F2+F3 G2+G3 A2+A3 D3+D3 F3+F3 A3+A3 D4 F4",
                                       "Two single top strings, octave strings on the eight basses; the basses are often retuned to the key of the piece")]),
        ("Viola da gamba", [
            T("Bass viol", "D2 G2 C3 E3 A3 D4"),
            T("Bass viol (7-string)", "A1 D2 G2 C3 E3 A3 D4"),
            T("Tenor viol", "G2 C3 F3 A3 D4 G4"),
            T("Treble viol", "D3 G3 C4 E4 A4 D5"),
            T("Violone in G", "G1 C2 F2 A2 D3 G3"),
        ]),
        ("Vihuela (Spanish)", [T("Standard", "G2+G2 C3+C3 F3+F3 A3+A3 D4+D4 G4", "Single top string, the others in pairs")]),
        ("Baroque guitar (5-course)", [
            T("Re-entrant (Spanish)", "A3+A3 D4+D4 G3+G3 B3+B3 E4", "No low strings: the 4th and 5th pairs are tuned high"),
            T("French (bourdon on the 4th)", "A3+A3 D3+D4 G3+G3 B3+B3 E4"),
            T("Bourdons on the 4th and 5th", "A2+A3 D3+D4 G3+G3 B3+B3 E4"),
        ]),
        ("Hurdy-gurdy", [T("G/C", "C3 G3 C4 G4+G4", "Drones (bourdon C, mouche G, trompette C), then the two melody strings on G. Drone setups vary by maker")]),
    ]),
    ("Folk and world strings", [
        ("Bouzouki (Irish)", [
            T("GDAD (octave low pairs)", "G2+G3 D3+D4 A3+A3 D4+D4", "Octave strings on the G and D pairs, a common setup"),
            T("GDAD (all pairs the same pitch)", "G2+G2 D3+D3 A3+A3 D4+D4"),
            T("GDAE", "G2+G3 D3+D4 A3+A3 E4+E4"),
            T("ADAD", "A2+A3 D3+D4 A3+A3 D4+D4"),
        ]),
        ("Bouzouki (Greek)", [
            T("Tetrachordo (4-course)", "C3+C4 F3+F4 A3+A3 D4+D4"),
            T("Trichordo (3-course)", "D3+D4 A3+A3 D4+D4"),
        ]),
        ("Oud", [
            T("Arabic", "C2 F2+F2 A2+A2 D3+D3 G3+G3 C4+C4", "Single low string, then five pairs"),
            T("Turkish", "E2 A2+A2 B2+B2 E3+E3 A3+A3 D4+D4"),
        ]),
        ("Appalachian dulcimer", [
            T("DAd (Ionian)", "D3 A3 D4"),
            T("DAdd (doubled melody string)", "D3 A3 D4+D4"),
            T("DAA (Mixolydian)", "D3 A3 A3"),
            T("DGD", "D3 G3 D4"),
            T("CGc", "C3 G3 C4"),
        ]),
        ("Balalaika (prima)", [T("Standard", "E4 E4 A4")]),
        ("Domra", [
            T("3-string", "E4 A4 D5"),
            T("4-string", "G3 D4 A4 E5"),
        ]),
        ("Charango", [T("Standard", "G4+G4 C5+C5 E4+E5 A4+A4 E5+E5", "Re-entrant: the middle pair has the lowest string, an octave under its partner")]),
        ("Cavaquinho", [T("Standard (DGBD)", "D4 G4 B4 D5")]),
        ("Cuatro (Venezuelan)", [T("Standard", "A3 D4 F#4 B3", "Re-entrant: the 1st string is lower than the 2nd")]),
        ("Cuatro (Puerto Rican)", [T("Standard", "B2+B3 E3+E4 A3+A3 D4+D4 G4+G4", "Octave strings on the B and E pairs")]),
        ("Tres (Cuban)", [
            T("Standard (C)", "G3+G4 C4+C4 E4+E5", "Octave strings on the G and E pairs"),
            T("Unison top pair", "G3+G4 C4+C4 E4+E4"),
            T("D tuning", "A3+A4 D4+D4 F#4+F#5"),
        ]),
        ("Tres (Puerto Rican)", [T("Standard", "G3+G4+G4 C4+C4+C4 E3+E4+E4", "Three groups of three strings")]),
        ("Vihuela (Mexican)", [T("Standard", "A3 D4 G4 B3 E4", "Re-entrant")]),
        ("Tiple (Colombian)", [
            T("Modern", "D3+D4+D4 G3+G4+G4 B3+B4+B4 E4+E4+E4", "Three groups of three: the middle string of the low three groups is an octave down"),
            T("Traditional", "C3+C4+C4 F3+F4+F4 A3+A4+A4 D4+D4+D4"),
        ]),
        ("Bandurria", [T("Standard", "G#3+G#3 C#4+C#4 F#4+F#4 B4+B4 E5+E5 A5+A5")]),
        ("Laud (Spanish)", [T("Standard", "G#2+G#2 C#3+C#3 F#3+F#3 B3+B3 E4+E4 A4+A4")]),
        ("Kantele (5-string)", [
            T("Minor", "D4 E4 F4 G4 A4"),
            T("Major", "D4 E4 F#4 G4 A4"),
        ]),
        ("Pipa", [T("Standard", "A2 D3 E3 A3")]),
        ("Zhongruan", [T("Standard", "G2 D3 G3 D4")]),
        ("Erhu", [T("Standard", "D4 A4")]),
        ("Shamisen", [
            T("Honchoshi", "C3 F3 C4", "Usually tuned to the singer: move it all up or down together"),
            T("Niagari", "C3 G3 C4"),
            T("Sansagari", "C3 F3 Bb3"),
        ]),
        ("Koto (13-string)", [T("Hirajoshi", "D3 G3 A3 Bb3 D4 Eb4 G4 A4 Bb4 D5 Eb5 G5 A5")]),
    ]),
    ("Winds and brass", [
        ("Flute", W("Bb4", "A4", "headjoint")),
        ("Piccolo", W("Bb5", "A5", "headjoint")),
        ("Oboe", W("Bb4", "A4", "Mostly the reed: push it further in to go sharper, ease it out a little to go flatter. Your embouchure does the rest.")),
        ("English horn (F)", W("Bb4", "A4", "Mostly the bocal and reed: push in to go sharper, ease out to go flatter.")),
        ("Clarinet (Bb)", W("Bb4", "A4", "barrel")),
        ("Clarinet (A)", W("Bb4", "A4", "barrel")),
        ("Bass clarinet (Bb)", W("Bb3", "A3", "Sharp: pull the neck out a little. Flat: push it in.")),
        ("Bassoon", W("Bb3", "A3", "Mostly the bocal and reed: a longer bocal (a higher number) plays flatter, a shorter one sharper. Your embouchure does the fine work.")),
        ("Soprano saxophone (Bb)", W("Bb4", "A4", "mouthpiece")),
        ("Alto saxophone (Eb)", W("Bb4", "A4", "mouthpiece")),
        ("Tenor saxophone (Bb)", W("Bb3", "A3", "mouthpiece")),
        ("Baritone saxophone (Eb)", W("Bb2", "A2", "mouthpiece")),
        ("Trumpet / cornet (Bb)", W("Bb4", "A4", "slide")),
        ("Flugelhorn (Bb)", W("Bb4", "A4", "slide")),
        ("French horn (F)", W("Bb3", "A3", "Sharp: pull the main tuning slide out. Flat: push it in. On a double horn, tune the F and Bb sides with their own slides.")),
        ("Mellophone (F)", W("Bb4", "A4", "slide")),
        ("Trombone", W("Bb3", "A3", "Sharp: pull the tuning slide (at the back) out. Flat: push it in. Concert Bb is first position.")),
        ("Euphonium / baritone horn", W("Bb3", "A3", "slide")),
        ("Tuba", W("Bb2", "A2", "slide")),
    ]),
    ("Percussion", [
        ("Timpani", [
            T("Pair: G and C", "G2 C3", "29\" and 26\" drums. Tap where you'd play, a hand's width in from the edge, and set each drum with its pedal"),
            T("Pair: A and D", "A2 D3", "29\" and 26\" drums"),
            T("Pair: F and Bb", "F2 Bb2", "29\" and 26\" drums"),
            T("Any note", "", "Usual ranges: 32\" D2 to A2, 29\" F2 to C3, 26\" Bb2 to F3, 23\" D3 to A3"),
        ]),
        ("Kalimba", [
            T("17-key in C", "D6 B5 G5 E5 C5 A4 F4 D4 C4 E4 G4 B4 D5 F5 A5 C6 E6", KALIMBA),
            T("10-key in C", "E5 C5 A4 F4 D4 C4 E4 G4 B4 D5", KALIMBA),
            T("Any note", "", KALIMBA),
        ]),
    ]),
    ("Anything else", [
        ("Chromatic (any instrument)", [T("Any note", "", "Voice, piano, harp, drums, handpan, didgeridoo, bagpipes...")]),
    ]),
]

# What the tuner calls the things it numbers: strings count from the high side (string 1 is the
# highest), anything else counts from the left as you see it; "" names nothing (one tuning note)
PART = {"Timpani": "drum", "Kalimba": "tine"}
def part(name, family):
    return "" if family == "Winds and brass" else PART.get(name, "string")

# written = concert + this many semitones
TRANSPOSE = {
    "Piccolo": -12, "English horn (F)": 7, "Clarinet (Bb)": 2, "Clarinet (A)": 3, "Bass clarinet (Bb)": 14,
    "Soprano saxophone (Bb)": 2, "Alto saxophone (Eb)": 9, "Tenor saxophone (Bb)": 14, "Baritone saxophone (Eb)": 21,
    "Trumpet / cornet (Bb)": 2, "Flugelhorn (Bb)": 2, "French horn (F)": 7, "Mellophone (F)": 7,
}

# Temperaments: each pitch class's offset from equal temperament in cents, built on C
# (C, C#, D, Eb, E, F, F#, G, G#, A, Bb, B); the tuner turns them to the chosen key.
TEMPERAMENTS = [
    ("Equal temperament", [0] * 12),
    ("Just intonation (major)", [0, 11.7, 3.9, 15.6, -13.7, -2.0, -9.8, 2.0, 13.7, -15.6, 17.6, -11.7]),
    ("Pythagorean", [0, 13.7, 3.9, -5.9, 7.8, -2.0, 11.7, 2.0, 15.6, 5.9, -3.9, 9.8]),
    ("Quarter-comma meantone", [0, -24.0, -6.8, 10.3, -13.7, 3.4, -20.5, -3.4, -27.4, -10.3, 6.8, -17.1]),
    ("Werckmeister III", [0, -9.8, -7.8, -5.9, -9.8, -2.0, -11.7, -3.9, -7.8, -11.7, -3.9, -7.8]),
    ("Kirnberger III", [0, -9.8, -6.8, -5.9, -13.7, -2.0, -9.8, -3.4, -7.8, -10.3, -3.9, -11.7]),
    ("Vallotti", [0, -3.9, -2.0, 0, -5.9, 2.0, -5.9, -2.0, -2.0, -3.9, 2.0, -5.9]),
]

REFERENCES = [("440 Hz (modern)", 440), ("442 Hz (European orchestras)", 442), ("443 Hz", 443), ("441 Hz", 441), ("432 Hz", 432),
              ("430 Hz (classical era)", 430), ("415 Hz (baroque)", 415), ("466 Hz (Chorton)", 466)]

NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

def midi(note):
    """C4 = 60"""
    letter, rest = note[0], note[1:]
    pc = NAMES[letter]
    while rest and rest[0] in "#b":
        pc += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    return pc + 12 * (int(rest) + 1)

def check():
    for family, instruments in CATALOG:
        for name, tunings in instruments:
            for t in tunings:
                for n in t["notes"]:
                    m = midi(n)
                    assert 20 <= m <= 100, (name, t["name"], n)
                assert sum(t["courses"]) == len(t["notes"]), (name, t["name"])

def cpp_str(s):
    out = '"'
    for b in s.encode("utf-8"):
        c = chr(b)
        if b >= 128: out += "\\%03o" % b
        elif c in ('"', "\\"): out += "\\" + c
        else: out += c
    return out + '"'

def write_cpp():
    lines = []
    for family, instruments in CATALOG:
        lines.append("            { %s, {" % cpp_str(family))
        for name, tunings in instruments:
            ts = ", ".join("{ %s, { %s }, { %s }, %s }" % (cpp_str(t["name"]), ", ".join(str(midi(n)) for n in t["notes"]),
                                                        ", ".join(str(c) for c in t["courses"]) if max(t["courses"], default=1) > 1 else "",
                                                        cpp_str(t["note"])) for t in tunings)
            lines.append("                { %s, %d, %s, { %s } }," % (cpp_str(name), TRANSPOSE.get(name, 0), cpp_str(part(name, family)), ts))
        lines.append("            } },")
    temps = ",\n".join("            { %s, { %s } }" % (cpp_str(n), ", ".join("%.1ff" % v for v in offs)) for n, offs in TEMPERAMENTS)
    refs = ", ".join("{ %s, %d }" % (cpp_str(n), v) for n, v in REFERENCES)
    body = open(os.path.join(HERE, "tunings_template.h"), encoding="utf-8").read()
    body = body.replace("@CATALOG@", "\n".join(lines)).replace("@TEMPERAMENTS@", temps).replace("@REFERENCES@", refs)
    open(os.path.join(ROOT, "dsp", "Tunings.h"), "w", encoding="utf-8", newline="\n").write(body)

def write_json():
    data = {
        "families": [
            {"name": family, "instruments": [
                {"name": name, "transpose": TRANSPOSE.get(name, 0), "part": part(name, family),
                 "tunings": [{"name": t["name"], "notes": t["notes"], "midi": [midi(n) for n in t["notes"]],
                              "courses": t["courses"], "note": t["note"]} for t in tunings]}
                for name, tunings in instruments]}
            for family, instruments in CATALOG],
        # whole numbers written without ".0", as JavaScript writes them
        "temperaments": [{"name": n, "cents": [int(v) if float(v).is_integer() else v for v in offs]} for n, offs in TEMPERAMENTS],
        "references": [{"name": n, "hz": v} for n, v in REFERENCES],
    }
    text = json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n"
    open(os.path.join(ROOT, "data", "tunings.json"), "w", encoding="utf-8", newline="\n").write(text)

if __name__ == "__main__":
    check()
    write_cpp()
    write_json()
    n = sum(len(i) for _, i in CATALOG)
    t = sum(len(ts) for _, i in CATALOG for _, ts in i)
    strings = sum(len(x["notes"]) for _, i in CATALOG for _, ts in i for x in ts)
    print(f"{n} instruments, {t} tunings, {strings} strings in all, {len(TEMPERAMENTS)} temperaments")

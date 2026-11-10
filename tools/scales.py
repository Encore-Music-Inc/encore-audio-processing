# ENCORE. Audio Processing
# Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
#
# The scale library: every scale ENCORE.'s products name, with what each is
# for, in one place. Running this writes the two files made from it:
#   dsp/ScaleLibrary.h   for C++
#   data/scales.json     for the browser
# CI runs it and fails if either file differs from what it writes.
#
#   python3 tools/scales.py
#
# Each scale: S(id, name, group, formula, ...). The formula is its degrees
# counted up the major scale from the root ("1 2 b3 4 5 6 b7"); the semitones
# and the rest are worked out from it.
#   short    a shorter name for tight spaces
#   aka      other names it goes by
#   char     the degree(s) that give it its flavour
#   near     its nearest relative (for "how it differs"), or None
#   prior    how often it's the everyday answer (lower is more common), to break ties
#   sym      a symmetric scale (spelled the plainest way, not one letter per degree)
#   jazz     the formula as jazz players read it over a chord ("b9", "#11")
#   approx   why it is only a 12-tone version of a scale tuned between our notes
#   differs  a hand-written "how it differs", in place of the worked-out comparison
#   note     a note about the name
#   mood, genres, why: what it sounds like, where it's heard, and why it sounds so

import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def S(id, name, group, formula, near, prior, short="", aka=(), char=(), sym=False, jazz="",
      approx="", differs="", note="", mood="", genres="", why=""):
    return {"id": id, "name": name, "group": group, "f": formula.split(), "short": short, "aka": list(aka),
            "char": list(char), "near": near, "prior": prior, "sym": sym, "jazz": jazz.split(), "approx": approx,
            "differs": differs, "note": note, "mood": mood, "genres": genres, "why": why}

# The library's sections, in the order they're listed
GROUPS = [
    ("major", "Major and its modes"),
    ("minor", "Minor scales"),
    ("hminor", "Modes of harmonic minor"),
    ("mminor", "Modes of melodic minor"),
    ("penta", "Pentatonic and blues"),
    ("bebop", "Bebop"),
    ("sym", "Symmetric"),
    ("world", "World and folk"),
]

SCALES = [
    # Major and its modes
    S("ionian", "Major (Ionian)", "major", "1 2 3 4 5 6 7",
      short="Major", aka=["Major", "Ionian"], char=["7"], near="mixolydian", prior=0,
      mood="Bright, settled, happy, finished.",
      genres="Pop, folk, country, hymns, classical, children's songs: most of the music you know.",
      why="Its 7th sits a half step under the root and pulls up to it (the leading tone), so melodies feel finished when they land home."),
    S("dorian", "Dorian", "major", "1 2 b3 4 5 6 b7",
      char=["6"], near="aeolian", prior=0,
      mood="Minor but hopeful: cool, soulful, a little sly.",
      genres="Funk, soul, modal jazz (Miles Davis' So What), Santana (Oye Como Va), Celtic and English folk (Scarborough Fair).",
      why="A minor scale with a major 6th. That one raised note brightens the minor sound and gives a major IV chord (Am to D), the groove of funk and Santana."),
    S("phrygian", "Phrygian", "major", "1 b2 b3 4 5 b6 b7",
      char=["b2"], near="aeolian", prior=0.7,
      mood="Dark, tense, brooding, Spanish-tinged.",
      genres="Flamenco, metal (thrash, death, djent), film scores.",
      why="Its 2nd is only a half step above the root, so melodies fall heavily onto home. A b2 over a minor chord is the darkest common sound in Western music."),
    S("lydian", "Lydian", "major", "1 2 3 #4 5 6 7",
      char=["#4"], near="ionian", prior=0.6,
      mood="Bright, dreamy, floating, full of wonder.",
      genres="Film and TV scores (flying, magic, wonder), Joe Satriani, Steve Vai, prog rock, jazz over maj7#11 chords.",
      why="The raised 4th removes the major scale's one clash (the 4th rubbing a half step above the 3rd), so nothing pulls down. It hovers."),
    S("mixolydian", "Mixolydian", "major", "1 2 3 4 5 6 b7",
      char=["b7"], near="ionian", prior=0.3,
      mood="Bright but bluesy: laid-back, rocking, earthy.",
      genres="Classic rock (Sweet Home Alabama), blues, country, bluegrass, Celtic and bagpipe tunes, funk, jam bands.",
      why="Major with a flat 7th. The home chord becomes a dominant 7th, and the bVII chord (G to F in G) gives rock and folk their swagger without the pull of a leading tone."),
    S("aeolian", "Natural minor (Aeolian)", "major", "1 2 b3 4 5 b6 b7",
      short="Natural minor", aka=["Natural minor", "Aeolian", "Minor"], char=["b6"], near="dorian", prior=0.15,
      mood="Sad, serious, reflective.",
      genres="Rock, pop ballads, metal, folk: the minor key of most songs.",
      why="The b3, b6 and b7 all sit lower than in major, which our ears hear as sad. With no leading tone it settles softly instead of pulling home."),
    S("locrian", "Locrian", "major", "1 b2 b3 4 b5 b6 b7",
      char=["b5"], near="phrygian", prior=0.4,
      mood="Unstable, eerie, unresolved.",
      genres="Rare as a song's home key. Heard in metal riffs, and in jazz over half-diminished (m7b5) chords.",
      why="Its 5th is flat, so its own home chord is diminished. Home itself sounds unsettled, which is why it is mostly used over one chord at a time."),
    # Minor scales
    S("harmonic_minor", "Harmonic minor", "minor", "1 2 b3 4 5 b6 7",
      char=["7"], near="aeolian", prior=0.5,
      mood="Dramatic, intense, classical, exotic.",
      genres="Classical, neoclassical metal (Yngwie Malmsteen), tango, East European and Middle Eastern flavored music.",
      why="Natural minor with its 7th raised, so the V chord turns major and pulls home hard. The gap from b6 up to 7 (a step and a half) gives its exotic edge."),
    S("melodic_minor", "Melodic minor (jazz minor)", "minor", "1 2 b3 4 5 6 7",
      aka=["Jazz minor", "Ascending melodic minor"], char=["6", "7"], near="dorian", prior=0.5,
      mood="Minor but smooth and sophisticated, bittersweet.",
      genres="Jazz (over m(maj7) and m6 chords), classical, film noir.",
      why="Major with a minor 3rd: the top half is pure major, so it is minor with a bright, rising pull. Classical players use it going up and natural minor coming down; jazz uses it both ways."),
    S("harmonic_major", "Harmonic major", "minor", "1 2 3 4 5 b6 7",
      char=["b6"], near="ionian", prior=1.2,
      mood="Major with a shadow: noble, bittersweet.",
      genres="Classical (Rimsky-Korsakov), film scores, jazz.",
      why="Major with a b6. The iv chord turns minor (the \"sad IV\") while the leading tone keeps its pull home."),
    # Modes of harmonic minor
    S("locrian_n6", "Locrian natural 6", "hminor", "1 b2 b3 4 b5 6 b7",
      aka=["Locrian #6"], char=["6"], near="locrian", prior=0.9,
      mood="Dark and tense, with an odd bright glint.",
      genres="Jazz over the iiø7 chord of a minor ii, V, i; metal.",
      why="Locrian with a major 6th: the same diminished home chord, but the raised 6th (the harmonic minor's leading tone) adds a strange light."),
    S("ionian_s5", "Ionian #5 (Ionian augmented)", "hminor", "1 2 3 4 #5 6 7",
      char=["#5"], near="ionian", prior=1,
      mood="Bright and restless, dreamy-strange.",
      genres="Jazz over maj7#5 chords, film music.",
      why="Major with a raised 5th, so the home chord is augmented and never quite settles."),
    S("ukrainian_dorian", "Ukrainian Dorian (Dorian #4)", "hminor", "1 2 b3 #4 5 6 b7",
      aka=["Dorian #4", "Romanian minor", "Misheberakh (klezmer)", "Altered Dorian", "Maqam Nikriz (Arabic and Turkish, 12-tone approximation)"], char=["#4"], near="dorian", prior=1,
      mood="Plaintive and yearning, with a sob in it.",
      genres="Klezmer (the Misheberakh mode), Ukrainian, Romanian and other East European folk, Jewish prayer chant, Greek and Turkish music.",
      why="Dorian with a raised 4th. The #4 leans up into the 5th, and the step and a half from b3 up to #4 is the crying \"augmented 2nd\" of East European melody."),
    S("phrygian_dominant", "Phrygian dominant (Freygish)", "hminor", "1 b2 3 4 5 b6 b7",
      aka=["Freygish (klezmer)", "Ahava Rabbah (Jewish prayer mode)", "Spanish Phrygian", "Maqam Hijaz (Arabic, 12-tone approximation)"], char=["b2", "3"], near="phrygian", prior=0.7,
      mood="Fiery, exotic, intense.",
      genres="Flamenco, klezmer (Freygish), Jewish cantorial music, Middle Eastern and Balkan music, metal.",
      why="Phrygian with a major 3rd. The step and a half from b2 up to 3 is the sound people call \"exotic\". It is the scale of the major V chord in a minor key (E7 in A minor)."),
    S("lydian_s2", "Lydian #2", "hminor", "1 #2 3 #4 5 6 7",
      char=["#2"], near="lydian", prior=1.4,
      mood="Bright, strange, shimmering.",
      genres="Jazz, fusion and film music; rare.",
      why="Lydian with a raised 2nd: the #2 crowds up against the major 3rd for a bright but uneasy color."),
    S("ultralocrian", "Altered diminished (Ultralocrian)", "hminor", "1 b2 b3 b4 b5 b6 bb7",
      aka=["Ultralocrian", "Super-Locrian bb7"], char=["bb7"], near="altered", prior=1.2,
      mood="Very dark, very tense.",
      genres="Jazz over diminished 7th chords; metal.",
      why="Its 4th is flat (it sounds like a major 3rd) and its 7th double flat (it sounds like a 6th), so it holds a diminished 7th chord on its root."),
    # Modes of melodic minor
    S("dorian_b2", "Dorian b2 (Phrygian natural 6)", "mminor", "1 b2 b3 4 5 6 b7",
      aka=["Phrygian #6"], char=["b2"], near="dorian", prior=0.9,
      mood="Dark and mysterious, lighter than Phrygian.",
      genres="Jazz over sus(b9) chords, modal jazz, metal.",
      why="Dorian with a flat 2nd: the b2 darkens it like Phrygian while the major 6th keeps a little light."),
    S("lydian_aug", "Lydian augmented", "mminor", "1 2 3 #4 #5 6 7",
      char=["#5"], near="lydian", prior=0.8,
      mood="Brighter than bright, mysterious, futuristic.",
      genres="Jazz over maj7#5 chords, film scores.",
      why="Lydian with a raised 5th as well: the floating #4 plus an augmented home chord that never settles."),
    S("lydian_dominant", "Lydian dominant", "mminor", "1 2 3 #4 5 6 b7",
      aka=["Overtone scale", "Acoustic scale", "Lydian b7", "Mixolydian #4"], char=["#4", "b7"], near="mixolydian", prior=0.7,
      mood="Bright, bluesy, playful, a little whimsical.",
      genres="Jazz over 7#11 chords, fusion, Brazilian music, Bartok, cartoon and film themes.",
      why="It is close to the overtones a single note naturally rings with, so it sounds \"right\" over a dominant chord: Mixolydian with its clashing 4th raised out of the way."),
    S("mixolydian_b6", "Mixolydian b6", "mminor", "1 2 3 4 5 b6 b7",
      aka=["Aeolian dominant", "Melodic major"], char=["b6"], near="mixolydian", prior=0.8,
      mood="Major but melancholy, wistful.",
      genres="Film music, rock and pop ballads, jazz over 7b13 chords.",
      why="Its lower half is major and its upper half minor: it starts bright and turns sad."),
    S("locrian_s2", "Locrian #2 (half-diminished)", "mminor", "1 2 b3 4 b5 b6 b7",
      aka=["Locrian natural 2", "Aeolian b5", "Half-diminished scale"], char=["2"], near="locrian", prior=0.3,
      mood="Dark and unstable, but smoother than Locrian.",
      genres="Jazz over m7b5 (half-diminished) chords.",
      why="Locrian with a natural 2nd: the same half-diminished home chord without the b2 grinding against the root."),
    S("altered", "Altered (super-Locrian)", "mminor", "1 b2 b3 b4 b5 b6 b7",
      aka=["Super-Locrian", "Diminished whole tone"], char=["b2", "b3", "b5", "b6"], near="locrian", prior=0.6, jazz="1 b9 #9 3 #11 b13 b7",
      mood="Maximum tension that melts into the next chord.",
      genres="Jazz and fusion over altered dominant (7alt) chords just before they resolve.",
      why="Over a dominant 7th it keeps the root, 3rd (spelled b4) and 7th, and every other note is an altered tension (b9, #9, #11, b13). Pure tension, so it resolves beautifully."),
    # Pentatonic and blues
    S("major_pent", "Major pentatonic", "penta", "1 2 3 5 6",
      near="ionian", prior=0.6,
      mood="Open, sunny, simple, can't-go-wrong.",
      genres="Country, folk, rock, gospel, children's songs, and music all over the world (Scotland, China, West Africa).",
      why="It leaves out the major scale's two half steps (the 4th and 7th), so no note clashes with any other. Every note sounds good over the home chord."),
    S("minor_pent", "Minor pentatonic", "penta", "1 b3 4 5 b7",
      near="aeolian", prior=0.5,
      mood="Bluesy, tough, soulful.",
      genres="Blues, rock and metal solos, R&B, funk, hip hop.",
      why="Natural minor without its 2nd and b6, so it has no half steps at all. Easy to bend and wail on: the first scale most guitarists learn."),
    S("blues", "Blues scale (minor blues)", "penta", "1 b3 4 b5 5 b7",
      aka=["Minor blues", "Blues"], char=["b5"], near="minor_pent", prior=0.8,
      mood="Gritty, bluesy, a little sly.",
      genres="Blues, rock, jazz, funk, soul.",
      why="Minor pentatonic plus the b5, the \"blue note\". It is a passing note: it rubs for a moment, then slides to the 4th or the 5th."),
    S("major_blues", "Major blues", "penta", "1 2 b3 3 5 6",
      char=["b3"], near="major_pent", prior=1,
      mood="Sweet, sunny and bluesy.",
      genres="Country, Western swing, bluegrass, blues, Southern rock.",
      why="Major pentatonic plus the b3, which you slide or hammer up to the major 3rd: the country and blues \"grace note\" sound."),
    # Bebop
    S("bebop_dominant", "Bebop dominant", "bebop", "1 2 3 4 5 6 b7 7",
      char=["7"], near="mixolydian", prior=1.1,
      mood="Swinging, flowing, jazzy.",
      genres="Bebop and swing (Charlie Parker), jazz lines over dominant 7th chords.",
      why="Mixolydian plus a passing major 7th. With eight notes, a run in 8th notes that starts on a chord tone on the beat keeps landing chord tones on the beat."),
    S("bebop_major", "Bebop major", "bebop", "1 2 3 4 5 #5 6 7",
      char=["#5"], near="ionian", prior=1.2,
      mood="Smooth, swinging, sophisticated.",
      genres="Jazz lines over major and 6th chords (Barry Harris writes the added note as a b6).",
      why="Major plus a passing #5. Running it in 8th notes from a chord tone puts the 6th chord's notes (1, 3, 5, 6) on every beat."),
    S("bebop_dorian", "Bebop minor (Dorian bebop)", "bebop", "1 2 b3 3 4 5 6 b7",
      aka=["Dorian bebop"], char=["3"], near="dorian", prior=1.2,
      mood="Cool, swinging, bluesy minor.",
      genres="Jazz lines over minor 7th chords (and the ii of a ii, V, I).",
      why="Dorian plus a passing major 3rd between the b3 and the 4th, so 8th-note runs keep the chord tones on the beat."),
    # Symmetric
    S("whole_tone", "Whole tone", "sym", "1 2 3 #4 #5 b7",
      near="lydian_dominant", prior=0.5, sym=True,
      mood="Dreamy, floating, hazy, unresolved.",
      genres="Debussy, TV dream sequences, Thelonious Monk, jazz over 7#5 chords.",
      why="Every step is a whole step, so no note is more \"home\" than another and nothing pulls anywhere. There are only two different whole-tone scales."),
    S("dim_hw", "Diminished (half-whole)", "sym", "1 b2 b3 3 #4 5 6 b7",
      aka=["Dominant diminished", "Octatonic (half-whole)"], char=["b2", "b3", "#4"], near="mixolydian", prior=0.6, sym=True, jazz="1 b9 #9 3 #11 5 13 b7",
      differs="Compared with Mixolydian: it keeps 1, 3, 5, 6 and b7 (a dominant 13th chord), but trades the 2nd for both a b2 and a #2 (b9 and #9) and raises the 4th to #4.",
      mood="Tense, angular, jazzy.",
      genres="Jazz over 7b9 chords, film tension, metal, Stravinsky and Bartok.",
      why="Half step, whole step, over and over: it repeats every 3 frets. Over a dominant 7th it gives b9, #9 and #11 while keeping the natural 5th and 13th."),
    S("dim_wh", "Diminished (whole-half)", "sym", "1 2 b3 4 b5 b6 6 7",
      aka=["Octatonic (whole-half)"], char=["2", "4", "b6", "7"], near="dim_hw", prior=0.4, sym=True,
      differs="Compared with half-whole diminished: the same alternating pattern, but starting with a whole step. That puts a diminished 7th chord (1 b3 b5 6) on its root instead of a dominant 7th.",
      mood="Mysterious, tense, symmetrical.",
      genres="Jazz over diminished 7th chords, film tension, metal.",
      why="Whole step, half step, over and over. It holds a diminished 7th chord on its root, with a note a whole step above each chord tone."),
    S("chromatic", "Chromatic", "sym", "1 b2 2 b3 3 4 #4 5 b6 6 b7 7",
      near=None, prior=99, sym=True,
      mood="No flavor of its own: all twelve notes.",
      genres="Every style uses chromatic passing notes; whole melodies built on it are modern classical and film.",
      why="Every half step. It is the set every other scale is chosen from, so it has no home of its own."),
    # World and folk
    S("hungarian_minor", "Hungarian minor", "world", "1 2 b3 #4 5 b6 7",
      aka=["Double harmonic minor"], char=["#4"], near="harmonic_minor", prior=1.2,
      note="Some older books call it the \"Gypsy minor\". Hungarian minor is the standard, respectful name.",
      mood="Dramatic, dark, passionate.",
      genres="Hungarian and Romani music, Liszt, flamenco-tinged and neoclassical metal.",
      why="Harmonic minor with a raised 4th: two step-and-a-half gaps (b3 to #4, b6 to 7), each landing a half step under a strong note."),
    S("hungarian_major", "Hungarian major", "world", "1 #2 3 #4 5 6 b7",
      char=["#2", "#4"], near="lydian_dominant", prior=1.5,
      mood="Bright, strange, festive.",
      genres="Liszt and Hungarian-inspired classical music; jazz over 7#9#11 chords.",
      why="A dominant 7th home chord with a #2 and a #4 around it: bright, but tilted."),
    S("double_harmonic", "Double harmonic major (Byzantine)", "world", "1 b2 3 4 5 b6 7",
      aka=["Byzantine", "Arabic", "Maqam Hijaz Kar (Arabic, 12-tone approximation)", "Raga Bhairav (North Indian, 12-tone approximation)"], char=["b2", "b6"], near="phrygian_dominant", prior=1.3,
      mood="Exotic, ornate, intense.",
      genres="Byzantine chant, Arabic, Turkish and Greek music, North Indian classical (Raga Bhairav), surf rock (Misirlou), desert scenes in film.",
      why="Two step-and-a-half gaps, each wrapped in half steps (b2 to 3, b6 to 7), so every strong note has a neighbor a half step away."),
    S("neapolitan_minor", "Neapolitan minor", "world", "1 b2 b3 4 5 b6 7",
      char=["b2"], near="harmonic_minor", prior=1.3,
      mood="Dark, operatic, dramatic.",
      genres="Classical and opera, metal.",
      why="Harmonic minor with a b2, so the dark Neapolitan chord (bII) lives right in the scale."),
    S("neapolitan_major", "Neapolitan major", "world", "1 b2 b3 4 5 6 7",
      char=["b2"], near="melodic_minor", prior=1.4,
      mood="Dark at the bottom, bright at the top.",
      genres="Classical and opera; jazz.",
      why="Melodic minor with a b2: a heavy start (1, b2, b3) that climbs into a bright major top half."),
    S("persian", "Persian", "world", "1 b2 3 4 b5 b6 7",
      char=["b5"], near="double_harmonic", prior=1.6,
      approx="A Western name. Persian classical music (the dastgah system) uses intervals that fall between our 12 notes, so this is a 12-tone flavor, not a real dastgah.",
      mood="Mysterious, tense, exotic.",
      genres="Western film and metal music reaching for a Persian color.",
      why="Double harmonic with a flat 5th: half steps everywhere, and a home chord that is neither major nor minor but tense."),
    S("hirajoshi", "Hirajoshi", "world", "1 2 b3 5 b6",
      near="aeolian", prior=1.4,
      note="Sources disagree on this name. This is the version most guitar books use.",
      mood="Haunting, spare, still.",
      genres="Japanese koto music as taught in the West, metal, film and game music.",
      why="Five notes with two half steps (2 to b3, 5 to b6) and two wide leaps: a sparse, bell-like sound."),
    S("in", "In (Miyako-bushi)", "world", "1 b2 4 5 b6",
      aka=["Miyako-bushi", "Sakura scale"], char=["b2"], near="phrygian", prior=1.4,
      mood="Melancholy and still.",
      genres="Japanese music for koto and shamisen (the folk song Sakura).",
      why="Two half steps (1 to b2, 5 to b6) and two big leaps: tender and sad."),
    S("insen", "Insen", "world", "1 b2 4 5 b7",
      char=["b2"], near="in", prior=1.5,
      mood="Dark, open, meditative.",
      genres="Japanese music, and Western film and game music wanting a Japanese color.",
      why="Like In, but with a b7 instead of a b6: one half step fewer, so it sounds more open."),
    S("iwato", "Iwato", "world", "1 b2 4 b5 b7",
      char=["b5"], near="locrian", prior=1.6,
      mood="Dark, eerie, unresolved.",
      genres="Japanese music; metal and film music.",
      why="Five notes from Locrian: the b2 and b5 make it one of the darkest pentatonic scales."),
    S("yo", "Yo", "world", "1 2 4 5 6",
      near="major_pent", prior=1.4,
      mood="Open, bright, folk-like.",
      genres="Japanese folk songs (min'yo) and court music.",
      why="No half steps, like major pentatonic, but with a 4th in place of the 3rd: open, neither major nor minor."),
    S("kumoi", "Kumoi", "world", "1 2 b3 5 6",
      near="dorian", prior=1.5,
      note="Names for Japanese scales vary between sources. This is the version most Western references use.",
      mood="Gentle, wistful.",
      genres="Japanese-flavored music; also used in jazz.",
      why="A minor 3rd and a major 6th with only one half step (2 to b3): soft minor color."),
    S("pelog", "Pelog (12-tone approximation)", "world", "1 b2 b3 5 b6",
      near="phrygian", prior=1.6,
      approx="Real pelog, from Javanese and Balinese gamelan, has seven uneven steps that differ from one gamelan to the next. This five-note set (close to pelog selisir) is the usual approximation on Western instruments.",
      mood="Shimmering, ancient, solemn.",
      genres="Javanese and Balinese gamelan (in its real tuning).",
      why="Small and large steps side by side (1, b2, b3, then a leap to 5), like the uneven steps of real pelog."),
    S("slendro", "Slendro (12-tone approximation)", "world", "1 2 4 5 b7",
      near="minor_pent", prior=1.6,
      approx="Real slendro, from gamelan, divides the octave into five nearly equal steps (about 240 cents each). No set of piano keys can do that, so any 12-tone version is a rough fit; this is a common one.",
      mood="Floating, calm, gong-like.",
      genres="Javanese and Balinese gamelan (in its real tuning).",
      why="Five notes with no half steps, as close as 12 notes get to five equal steps."),
]

# The major modes in order of brightness, brightest first (each one darker by a single lowered note)
BRIGHTNESS = ["lydian", "ionian", "mixolydian", "dorian", "aeolian", "phrygian", "locrian"]

# Families of modes: mode 1, 2, ... built on each degree of the first
FAMILIES = [
    ("major", "major", ["ionian", "dorian", "phrygian", "lydian", "mixolydian", "aeolian", "locrian"]),
    ("harmonic_minor", "harmonic minor", ["harmonic_minor", "locrian_n6", "ionian_s5", "ukrainian_dorian", "phrygian_dominant", "lydian_s2", "ultralocrian"]),
    ("melodic_minor", "melodic minor", ["melodic_minor", "dorian_b2", "lydian_aug", "lydian_dominant", "mixolydian_b6", "locrian_s2", "altered"]),
]

# The roots a player picks from: every note with at most one sharp or flat that is commonly used
ROOTS = ["C", "C#", "Db", "D", "D#", "Eb", "E", "F", "F#", "Gb", "G", "G#", "Ab", "A", "A#", "Bb", "B"]

MAJOR = [0, 2, 4, 5, 7, 9, 11]

def degree(d):
    """A degree like "b7" or "#11": its number, scale step (0..6), alteration and size in semitones"""
    m = re.fullmatch(r"(bb|b|#)?(\d+)", d)
    assert m, "not a degree: " + d
    acc = {"bb": -2, "b": -1, "#": 1}.get(m.group(1), 0)
    n = int(m.group(2))
    step = (n - 1) % 7
    return {"d": d, "n": n, "step": step, "acc": acc, "semis": MAJOR[step] + acc + 12 * ((n - 1) // 7)}

def worked(s):
    tones = [degree(d) for d in s["f"]]
    return tones, [t["semis"] % 12 for t in tones], len(tones), len(tones) == 7 and len({t["step"] for t in tones}) == 7

def check():
    ids = [s["id"] for s in SCALES]
    assert len(set(ids)) == len(ids), "an id is used twice"
    groups = [g for g, _ in GROUPS]
    for s in SCALES:
        assert s["group"] in groups, s["id"]
        assert s["near"] is None or s["near"] in ids, s["id"]
        assert s["mood"] and s["genres"] and s["why"], s["id"]
        tones, semis, _, _ = worked(s)
        assert semis[0] == 0 and semis == sorted(set(semis)), s["id"] + ": its semitones must rise from 0"
        assert not s["jazz"] or len(s["jazz"]) == len(s["f"]), s["id"]
        for t in s["jazz"]: degree(t)
    for i in BRIGHTNESS + [m for _, _, modes in FAMILIES for m in modes]:
        assert i in ids, i
    for _, _, modes in FAMILIES:
        first = worked(next(s for s in SCALES if s["id"] == modes[0]))[1]
        for k, m in enumerate(modes):
            got = worked(next(s for s in SCALES if s["id"] == m))[1]
            assert got == sorted((x - first[k]) % 12 for x in first), m + " is not mode %d of %s" % (k + 1, modes[0])

def number(v):
    # whole numbers written without ".0", as JavaScript writes them
    return int(v) if float(v).is_integer() else v

def write_json():
    scales = []
    for s in SCALES:
        tones, semis, size, hepta = worked(s)
        e = {"id": s["id"], "name": s["name"], "group": s["group"], "f": s["f"], "aka": s["aka"], "char": s["char"],
             "prior": number(s["prior"]), "approx": s["approx"], "sym": s["sym"], "near": s["near"]}
        if s["short"]: e["short"] = s["short"]
        if s["jazz"]: e["jazz"] = s["jazz"]
        if s["differs"]: e["differs"] = s["differs"]
        if s["note"]: e["note"] = s["note"]
        e.update({"mood": s["mood"], "genres": s["genres"], "why": s["why"], "tones": tones, "semis": semis, "size": size, "hepta": hepta})
        scales.append(e)
    data = {
        "groups": [{"id": g, "name": n} for g, n in GROUPS],
        "scales": scales,
        "brightness": BRIGHTNESS,
        "families": [{"id": i, "name": n, "modes": m} for i, n, m in FAMILIES],
        "roots": ROOTS,
    }
    text = json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n"
    open(os.path.join(ROOT, "data", "scales.json"), "w", encoding="utf-8", newline="\n").write(text)

def cpp_str(s):
    out = '"'
    for b in s.encode("utf-8"):
        c = chr(b)
        if b >= 128: out += "\\%03o" % b
        elif c in ('"', "\\"): out += "\\" + c
        else: out += c
    return out + '"'

def cpp_strs(name, xs):
    """a span over a named array of strings (an empty span when there are none)"""
    if not xs: return "{}", None
    return name, "        inline constexpr const char* %s[] { %s };" % (name, ", ".join(cpp_str(x) for x in xs))

HEADER = """// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The scale library: every scale ENCORE.'s products name, with its formula,
// semitones, family and the words shown with it. Made by tools/scales.py (edit
// the list there and run it, not this file). Text is UTF-8. Everything is
// constant data, so reading it never allocates (safe on an audio thread).
#pragma once

#include <array>
#include <span>
#include <string_view>

namespace vs::scales
{
    // A degree of a formula: its text ("b7"), number, scale step (0..6),
    // alteration (-2..1) and semitones above the root
    struct Degree { const char* d; int n; int step; int acc; int semis; };

    struct LibraryScale
    {
        const char* id;
        const char* name;
        const char* shortName;                  // a shorter name for tight spaces ("" when the name is short enough)
        const char* group;                      // the id of one of groups
        std::span<const Degree> tones;          // the formula, counted up the major scale from the root
        std::span<const int> semis;             // semitones of each degree above the root (0..11)
        bool hepta;                             // seven notes on seven different letters
        bool sym;                               // symmetric: spelled the plainest way, not one letter per degree
        std::span<const char* const> aka;       // other names it goes by
        std::span<const char* const> character; // the degree(s) that give it its flavour
        const char* nearId;                     // its nearest relative's id, or nullptr (for "how it differs")
        double prior;                           // how often it's the everyday answer (lower is more common)
        std::span<const char* const> jazz;      // the formula as jazz players read it over a chord (may be empty)
        const char* approx;                     // why it is only a 12-tone version of a scale tuned between our notes, or ""
        const char* differs;                    // a hand-written "how it differs", or ""
        const char* note;                       // a note about the name, or ""
        const char* mood;
        const char* genres;
        const char* why;
    };

    struct Group { const char* id; const char* name; };
    struct Family { const char* id; const char* name; std::array<const char*, 7> modes; };

    namespace detail
    {
@DATA@
    }

    // The library's sections, in the order they're listed
    inline constexpr std::array<Group, @NGROUPS@> groups { {
@GROUPS@
    } };

    inline constexpr std::array<LibraryScale, @NSCALES@> library { {
@SCALES@
    } };

    // The major modes in order of brightness, brightest first
    inline constexpr std::array<const char*, @NBRIGHT@> brightness { @BRIGHTNESS@ };

    // Families of modes: mode 1, 2, ... built on each degree of the first
    inline constexpr std::array<Family, @NFAMILIES@> families { {
@FAMILIES@
    } };

    // The roots a player picks from: every note with at most one sharp or flat that is commonly used
    inline constexpr std::array<const char*, @NROOTS@> roots { @ROOTS@ };

    // The scale with this id, or nullptr
    constexpr const LibraryScale* find (std::string_view id)
    {
        for (auto& s : library) if (id == s.id) return &s;
        return nullptr;
    }
}
"""

def write_cpp():
    data, rows = [], []
    for s in SCALES:
        tones, semis, size, hepta = worked(s)
        v = s["id"]
        data.append("        inline constexpr Degree tones_%s[] { %s };" % (v, ", ".join(
            "{ %s, %d, %d, %d, %d }" % (cpp_str(t["d"]), t["n"], t["step"], t["acc"], t["semis"]) for t in tones)))
        data.append("        inline constexpr int semis_%s[] { %s };" % (v, ", ".join(str(x) for x in semis)))
        spans = {}
        for field in ("aka", "char", "jazz"):
            ref, line = cpp_strs("%s_%s" % (field, v), s[field])
            spans[field] = ("detail::" + ref) if line else ref
            if line: data.append(line)
        rows.append("        { %s, %s, %s, %s, detail::tones_%s, detail::semis_%s, %s, %s,\n"
                    "          %s, %s, %s, %s, %s,\n"
                    "          %s,\n          %s,\n          %s,\n          %s,\n          %s,\n          %s },"
                    % (cpp_str(v), cpp_str(s["name"]), cpp_str(s["short"]), cpp_str(s["group"]), v, v,
                       "true" if hepta else "false", "true" if s["sym"] else "false",
                       spans["aka"], spans["char"], "nullptr" if s["near"] is None else cpp_str(s["near"]),
                       repr(float(s["prior"])), spans["jazz"],
                       cpp_str(s["approx"]), cpp_str(s["differs"]), cpp_str(s["note"]),
                       cpp_str(s["mood"]), cpp_str(s["genres"]), cpp_str(s["why"])))
    body = HEADER
    for k, v in {
        "@DATA@": "\n".join(data),
        "@NGROUPS@": str(len(GROUPS)),
        "@GROUPS@": "\n".join("        { %s, %s }," % (cpp_str(g), cpp_str(n)) for g, n in GROUPS),
        "@NSCALES@": str(len(SCALES)),
        "@SCALES@": "\n".join(rows),
        "@NBRIGHT@": str(len(BRIGHTNESS)),
        "@BRIGHTNESS@": ", ".join(cpp_str(x) for x in BRIGHTNESS),
        "@NFAMILIES@": str(len(FAMILIES)),
        "@FAMILIES@": "\n".join("        { %s, %s, { %s } }," % (cpp_str(i), cpp_str(n), ", ".join(cpp_str(m) for m in modes)) for i, n, modes in FAMILIES),
        "@NROOTS@": str(len(ROOTS)),
        "@ROOTS@": ", ".join(cpp_str(x) for x in ROOTS),
    }.items():
        body = body.replace(k, v)
    open(os.path.join(ROOT, "dsp", "ScaleLibrary.h"), "w", encoding="utf-8", newline="\n").write(body)

if __name__ == "__main__":
    check()
    write_cpp()
    write_json()
    print(f"{len(SCALES)} scales in {len(GROUPS)} groups, {len(FAMILIES)} families of modes")

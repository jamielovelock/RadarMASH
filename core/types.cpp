// RadarMASH - open firmware for the GeekMagic Radar 360.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plain names for the four-character type codes aircraft report (A388 is an A380-800). The codes are the ICAO type
// designators; the names are the makers' ordinary model names, kept short enough for the tag box. A type that is not
// here is simply shown by its code.
#include "radar.h"
#include <string.h>

namespace rm {

static const struct { const char *code, *name; } TYPE_NAMES[] = {
    // Airbus
    {"A19N", "A319neo"}, {"A20N", "A320neo"}, {"A21N", "A321neo"}, {"A318", "A318"}, {"A319", "A319"}, {"A320", "A320"},
    {"A321", "A321"}, {"A306", "A300-600"}, {"A30B", "A300B4"}, {"A310", "A310"}, {"A332", "A330-200"}, {"A333", "A330-300"},
    {"A338", "A330-800"}, {"A339", "A330-900"}, {"A342", "A340-200"}, {"A343", "A340-300"}, {"A345", "A340-500"},
    {"A346", "A340-600"}, {"A359", "A350-900"}, {"A35K", "A350-1000"}, {"A388", "A380-800"}, {"BCS1", "A220-100"},
    {"BCS3", "A220-300"}, {"A3ST", "Beluga"}, {"A337", "Beluga XL"}, {"A400", "A400M Atlas"},
    // Boeing and McDonnell Douglas
    {"B37M", "737 MAX 7"}, {"B38M", "737 MAX 8"}, {"B39M", "737 MAX 9"}, {"B3XM", "737 MAX 10"}, {"B712", "717-200"},
    {"B722", "727-200"}, {"B732", "737-200"}, {"B733", "737-300"}, {"B734", "737-400"}, {"B735", "737-500"}, {"B736", "737-600"},
    {"B737", "737-700"}, {"B738", "737-800"}, {"B739", "737-900"}, {"B741", "747-100"}, {"B742", "747-200"}, {"B744", "747-400"},
    {"B748", "747-8"}, {"B74S", "747SP"}, {"BLCF", "Dreamlifter"}, {"B752", "757-200"}, {"B753", "757-300"}, {"B762", "767-200"},
    {"B763", "767-300"}, {"B764", "767-400"}, {"B772", "777-200"}, {"B77L", "777-200LR"}, {"B773", "777-300"},
    {"B77W", "777-300ER"}, {"B778", "777-8"}, {"B779", "777-9"}, {"B788", "787-8"}, {"B789", "787-9"}, {"B78X", "787-10"},
    {"MD11", "MD-11"}, {"MD82", "MD-82"}, {"MD83", "MD-83"}, {"MD88", "MD-88"}, {"DC10", "DC-10"}, {"DC3", "DC-3"},
    // Regional airliners
    {"E170", "E170"}, {"E75L", "E175"}, {"E75S", "E175"}, {"E190", "E190"}, {"E195", "E195"}, {"E290", "E190-E2"},
    {"E295", "E195-E2"}, {"E135", "ERJ-135"}, {"E145", "ERJ-145"}, {"CRJ2", "CRJ-200"}, {"CRJ7", "CRJ-700"},
    {"CRJ9", "CRJ-900"}, {"CRJX", "CRJ-1000"}, {"DH8A", "Dash 8-100"}, {"DH8B", "Dash 8-200"}, {"DH8C", "Dash 8-300"},
    {"DH8D", "Dash 8-400"}, {"AT43", "ATR 42-300"}, {"AT45", "ATR 42-500"}, {"AT46", "ATR 42-600"}, {"AT72", "ATR 72"},
    {"AT75", "ATR 72-500"}, {"AT76", "ATR 72-600"}, {"SF34", "Saab 340"}, {"SB20", "Saab 2000"}, {"RJ85", "Avro RJ85"},
    {"RJ1H", "Avro RJ100"}, {"B461", "BAe 146-100"}, {"B462", "BAe 146-200"}, {"B463", "BAe 146-300"}, {"JS32", "Jetstream 32"},
    {"JS41", "Jetstream 41"}, {"F70", "Fokker 70"}, {"F100", "Fokker 100"}, {"D328", "Dornier 328"}, {"SU95", "Superjet 100"},
    {"DHC6", "Twin Otter"}, {"BN2P", "Islander"}, {"L410", "Let 410"},
    // Business jets and turboprops
    {"E35L", "Legacy 600"}, {"E50P", "Phenom 100"}, {"E55P", "Phenom 300"}, {"E550", "Legacy 500"}, {"CL30", "Challenger 300"},
    {"CL35", "Challenger 350"}, {"CL60", "Challenger 600"}, {"GLEX", "Global Express"}, {"GL5T", "Global 5000"},
    {"GL7T", "Global 7500"}, {"GLF4", "Gulfstream IV"}, {"GLF5", "Gulfstream V"}, {"GLF6", "Gulfstream 650"}, {"LJ35", "Learjet 35"},
    {"LJ45", "Learjet 45"}, {"LJ60", "Learjet 60"}, {"LJ75", "Learjet 75"}, {"C25A", "Citation CJ2"}, {"C25B", "Citation CJ3"},
    {"C25C", "Citation CJ4"}, {"C510", "Citation M."}, {"C525", "CitationJet"}, {"C550", "Citation II"}, {"C560", "Citation V"},
    {"C56X", "Citation XLS"}, {"C680", "Citation Sov."}, {"C68A", "Citation Lat."}, {"C700", "Citation Lon."}, {"C750", "Citation X"},
    {"F2TH", "Falcon 2000"}, {"F900", "Falcon 900"}, {"FA50", "Falcon 50"}, {"FA7X", "Falcon 7X"}, {"FA8X", "Falcon 8X"},
    {"H25B", "Hawker 800"}, {"PRM1", "Premier I"}, {"PC12", "Pilatus PC-12"}, {"PC24", "Pilatus PC-24"}, {"BE20", "King Air 200"},
    {"B350", "King Air 350"}, {"BE9L", "King Air 90"}, {"TBM9", "TBM 900"}, {"C208", "Caravan"},
    // Light aircraft
    {"C150", "Cessna 150"}, {"C152", "Cessna 152"}, {"C172", "Cessna 172"}, {"C182", "Cessna 182"}, {"C206", "Cessna 206"},
    {"P28A", "Piper PA-28"}, {"PA31", "Piper Navajo"}, {"PA34", "Piper Seneca"}, {"PA46", "Piper Malibu"}, {"SR20", "Cirrus SR20"},
    {"SR22", "Cirrus SR22"}, {"DA40", "Diamond DA40"}, {"DA42", "Diamond DA42"},
    // Helicopters
    {"EC35", "Airbus H135"}, {"EC45", "Airbus H145"}, {"EC30", "Airbus H130"}, {"EC55", "Airbus H155"}, {"EC25", "Airbus H225"},
    {"AS50", "AS350 Squirrel"}, {"AS55", "AS355 Squirrel"}, {"AS65", "Dauphin"}, {"A109", "AW109"}, {"A139", "AW139"}, {"A169", "AW169"},
    {"A189", "AW189"}, {"R22", "Robinson R22"}, {"R44", "Robinson R44"}, {"R66", "Robinson R66"}, {"B06", "Bell 206"},
    {"B407", "Bell 407"}, {"B429", "Bell 429"}, {"S76", "Sikorsky S-76"}, {"S92", "Sikorsky S-92"}, {"EXPL", "MD Explorer"},
    {"EH10", "Merlin"}, {"H47", "Chinook"}, {"H60", "Black Hawk"}, {"H64", "Apache"}, {"V22", "Osprey"},
    // Military, heavy lift and historic
    {"A124", "An-124"}, {"A225", "An-225"}, {"C17", "C-17"}, {"C5M", "C-5M Galaxy"}, {"C130", "C-130"}, {"C30J", "C-130J"},
    {"K35R", "KC-135R"}, {"E3TF", "E-3 Sentry"}, {"E3CF", "E-3 Sentry"}, {"B52", "B-52"}, {"U2", "U-2"}, {"P8", "P-8 Poseidon"},
    {"EUFI", "Typhoon"}, {"F35", "F-35"}, {"F15", "F-15"}, {"F16", "F-16"}, {"F18", "F/A-18"}, {"A10", "A-10"}, {"HAWK", "Hawk"},
    {"TEX2", "Texan II"}, {"SPIT", "Spitfire"}, {"HURI", "Hurricane"}, {"LANC", "Lancaster"},
};
const int TYPE_NAME_COUNT = (int)(sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]);

const char *typeCodeAt(int i) { return i >= 0 && i < TYPE_NAME_COUNT ? TYPE_NAMES[i].code : nullptr; }

const char *typeName(const char *code) {
  if (!code || !code[0]) return nullptr;
  for (int i = 0; i < TYPE_NAME_COUNT; i++)
    if (strcmp(TYPE_NAMES[i].code, code) == 0) return TYPE_NAMES[i].name;
  return nullptr;
}

const char *typeShown(const Settings &s, const char *code) {
  const char *name = s.type_names ? typeName(code) : nullptr;
  return name ? name : code;
}

}  // namespace rm

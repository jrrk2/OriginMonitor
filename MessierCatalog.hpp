// MessierCatalog.h - Messier objects converted to SkyPosition format
#ifndef MESSIERCATALOG_H
#define MESSIERCATALOG_H

#include <QStringList>
#include <QMap>

struct MessierSkyPosition {
    double ra_deg;
    double dec_deg;
    QString name;
    QString description;
};

// Object type enumeration
enum class MessierObjectType {
    GLOBULAR_CLUSTER,
    OPEN_CLUSTER, 
    NEBULA,
    PLANETARY_NEBULA,
    SUPERNOVA_REMNANT,
    GALAXY,
    GALAXY_CLUSTER,
    DOUBLE_STAR,
    ASTERISM,
    STAR_CLOUD,
    OTHER
};

// Constellation enumeration
enum class Constellation {
    ANDROMEDA, AQUARIUS, AURIGA, CANCER, CANES_VENATICI, CANIS_MAJOR,
    CAPRICORNUS, CASSIOPEIA, CETUS, COMA_BERENICES, CYGNUS, DRACO,
    GEMINI, HERCULES, HYDRA, LEO, LEPUS, LYRA, MONOCEROS, OPHIUCHUS,
    ORION, PEGASUS, PERSEUS, PISCES, PUPPIS, SAGITTA, SAGITTARIUS,
    SCORPIUS, SCUTUM, SERPENS, TAURUS, TRIANGULUM, URSA_MAJOR,
    VIRGO, VULPECULA
};

// Messier object structure
struct MessierObject {
    int id;
    QString name;
    QString common_name;
    MessierObjectType object_type;
    Constellation constellation;
    MessierSkyPosition sky_position;  // Converted from RA hours/Dec degrees
    float magnitude;
    float distance_kly;
    QSizeF size_arcmin;  // width, height in arcminutes
    QString description;
    QString best_viewed;
    bool has_been_imaged;  // From your imaged list
};

class MessierCatalog {
public:
    static QList<MessierObject> getAllObjects();
    static MessierObject getObjectById(int id);
    static QList<MessierObject> getImagedObjects();
    static QList<MessierObject> getObjectsByType(MessierObjectType type);
    static QList<MessierObject> getObjectsByConstellation(Constellation constellation);
    static QStringList getObjectNames();
    static QString objectTypeToString(MessierObjectType type);
    static QString constellationToString(Constellation constellation);
    
private:
    static QList<MessierObject> m_catalog;
    static void initializeCatalog();
    static MessierSkyPosition createSkyPosition(double ra_hours, double dec_degrees, 
                                       const QString& name, const QString& description);
};

// Convert RA hours to degrees  
inline double raHoursToDegrees(double ra_hours) {
    return ra_hours * 15.0;  // 1 hour = 15 degrees
}

// Initialize the catalog with converted coordinates
QList<MessierObject> MessierCatalog::m_catalog;

void MessierCatalog::initializeCatalog() {
    if (!m_catalog.isEmpty()) return;
    
    // Set of imaged objects for quick lookup
    QSet<QString> imaged_objects = {
        "M1", "M3", "M13", "M16", "M17", "M27", "M45", "M51", 
        "M74", "M81", "M101", "M106", "M109"
    };
    
    m_catalog = {
        {1, "M1", "Crab Nebula", MessierObjectType::SUPERNOVA_REMNANT, Constellation::TAURUS,
	createSkyPosition(5.575556, 22.013333, "M1", "Remains of a supernova observed in 1054 AD"),
	20.f, 6.5f, QSizeF(6., 4.), "Remains of a supernova observed in 1054 AD", "Winter",
	imaged_objects.contains("M1")},

        {2, "M2", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::AQUARIUS,
	createSkyPosition(21.557506, -0.82325, "M2", "One of the richest and most compact globular clusters"),
	6.2f, 37.5f, QSizeF(16., 16.), "One of the richest and most compact globular clusters", "Autumn",
	imaged_objects.contains("M2")},

        {3, "M3", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::CANES_VENATICI,
	createSkyPosition(13.703228, 28.377278, "M3", "Contains approximately 500,000 stars"),
	6.4f, 33.9f, QSizeF(18., 18.), "Contains approximately 500,000 stars", "Spring",
	imaged_objects.contains("M3")},

        {4, "M4", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SCORPIUS,
	createSkyPosition(16.393117, -26.52575, "M4", "One of the closest globular clusters to Earth"),
	20.f, 7.2f, QSizeF(26., 26.), "One of the closest globular clusters to Earth", "Summer",
	imaged_objects.contains("M4")},

        {5, "M5", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SERPENS,
	createSkyPosition(15.309228, 2.081028, "M5", "One of the older globular clusters in the Milky Way"),
	6.f, 24.5f, QSizeF(20., 20.), "One of the older globular clusters in the Milky Way", "Summer",
	imaged_objects.contains("M5")},

        {6, "M6", "Butterfly Cluster", MessierObjectType::OPEN_CLUSTER, Constellation::SCORPIUS,
	createSkyPosition(17.671389, -32.241667, "M6", "Contains about 80 stars visible with binoculars"),
	20.f, 1.6f, QSizeF(25., 25.), "Contains about 80 stars visible with binoculars", "Summer",
	imaged_objects.contains("M6")},

        {7, "M7", "Ptolemy's Cluster", MessierObjectType::OPEN_CLUSTER, Constellation::SCORPIUS,
	createSkyPosition(17.896389, -34.841667, "M7", "Mentioned by Ptolemy in 130 AD, visible to naked eye"),
	20.f, 0.8f, QSizeF(80., 80.), "Mentioned by Ptolemy in 130 AD, visible to naked eye", "Summer",
	imaged_objects.contains("M7")},

        {8, "M8", "Lagoon Nebula", MessierObjectType::NEBULA, Constellation::SAGITTARIUS,
	createSkyPosition(18.060278, -24.386667, "M8", "Contains a distinctive hourglass-shaped structure"),
	20.f, 4.1f, QSizeF(90., 40.), "Contains a distinctive hourglass-shaped structure", "Summer",
	imaged_objects.contains("M8")},

        {9, "M9", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(17.319939, -18.51625, "M9", "Located near the center of the Milky Way"),
	8.4f, 25.8f, QSizeF(9.3, 9.3), "Located near the center of the Milky Way", "Summer",
	imaged_objects.contains("M9")},

        {10, "M10", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(16.952514, -4.100306, "M10", "One of the brighter globular clusters visible from Earth"),
	5.f, 14.3f, QSizeF(20., 20.), "One of the brighter globular clusters visible from Earth", "Summer",
	imaged_objects.contains("M10")},

        {11, "M11", "Wild Duck Cluster", MessierObjectType::OPEN_CLUSTER, Constellation::SCUTUM,
	createSkyPosition(18.851111, -6.271667, "M11", "Resembles a flight of wild ducks in formation"),
	5.8f, 6.2f, QSizeF(14., 14.), "Resembles a flight of wild ducks in formation", "Summer",
	imaged_objects.contains("M11")},

        {12, "M12", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(16.787272, -1.948528, "M12", "Located in the constellation Ophiuchus"),
	6.1f, 16.f, QSizeF(16., 16.), "Located in the constellation Ophiuchus", "Summer",
	imaged_objects.contains("M12")},

        {13, "M13", "Hercules Globular Cluster", MessierObjectType::GLOBULAR_CLUSTER, Constellation::HERCULES,
	createSkyPosition(16.694898, 36.461319, "M13", "Contains several hundred thousand stars"),
	5.8f, 22.2f, QSizeF(20., 20.), "Contains several hundred thousand stars", "Summer",
	imaged_objects.contains("M13")},

        {14, "M14", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(17.626708, -3.245917, "M14", "One of the more distant globular clusters from Earth"),
	5.7f, 30.3f, QSizeF(11., 11.), "One of the more distant globular clusters from Earth", "Summer",
	imaged_objects.contains("M14")},

        {15, "M15", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::PEGASUS,
	createSkyPosition(21.499536, 12.167, "M15", "One of the oldest known globular clusters"),
	20.f, 33.6f, QSizeF(18., 18.), "One of the oldest known globular clusters", "Autumn",
	imaged_objects.contains("M15")},

        {16, "M16", "Eagle Nebula", MessierObjectType::OPEN_CLUSTER, Constellation::SERPENS,
	createSkyPosition(18.3125, -13.791667, "M16", "Contains the famous 'Pillars of Creation'"),
	6.f, 7.f, QSizeF(35., 28.), "Contains the famous 'Pillars of Creation'", "Summer",
	imaged_objects.contains("M16")},

        {17, "M17", "Omega Nebula", MessierObjectType::NEBULA, Constellation::SAGITTARIUS,
	createSkyPosition(18.346389, -16.171667, "M17", "Also known as the Swan Nebula or Horseshoe Nebula"),
	20.f, 5.f, QSizeF(11., 11.), "Also known as the Swan Nebula or Horseshoe Nebula", "Summer",
	imaged_objects.contains("M17")},

        {18, "M18", "", MessierObjectType::OPEN_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.3325, -17.088333, "M18", "Located in Sagittarius, near other famous deep sky objects"),
	20.f, 4.9f, QSizeF(9., 9.), "Located in Sagittarius, near other famous deep sky objects", "Summer",
	imaged_objects.contains("M18")},

        {19, "M19", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(17.043803, -26.267944, "M19", "One of the most oblate (flattened) globular clusters"),
	5.6f, 28.7f, QSizeF(17., 17.), "One of the most oblate (flattened) globular clusters", "Summer",
	imaged_objects.contains("M19")},

        {20, "M20", "Trifid Nebula", MessierObjectType::NEBULA, Constellation::SAGITTARIUS,
	createSkyPosition(18.045, -22.971667, "M20", "Has a distinctive three-lobed appearance"),
	20.f, 5.2f, QSizeF(28., 28.), "Has a distinctive three-lobed appearance", "Summer",
	imaged_objects.contains("M20")},

        {21, "M21", "", MessierObjectType::OPEN_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.069167, -22.505, "M21", "A relatively young open cluster of stars"),
	20.f, 4.2f, QSizeF(13., 13.), "A relatively young open cluster of stars", "Summer",
	imaged_objects.contains("M21")},

        {22, "M22", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.60665, -23.90475, "M22", "One of the brightest globular clusters visible from Earth"),
	6.2f, 10.4f, QSizeF(24., 24.), "One of the brightest globular clusters visible from Earth", "Summer",
	imaged_objects.contains("M22")},

        {23, "M23", "", MessierObjectType::OPEN_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(17.949167, -18.986667, "M23", "Contains about 150 stars visible with a small telescope"),
	20.f, 2.1f, QSizeF(27., 27.), "Contains about 150 stars visible with a small telescope", "Summer",
	imaged_objects.contains("M23")},

        {24, "M24", "Sagittarius Star Cloud", MessierObjectType::STAR_CLOUD, Constellation::SAGITTARIUS,
	createSkyPosition(18.28, -18.55, "M24", "A dense part of the Milky Way galaxy"),
	20.f, 10.f, QSizeF(90., 90.), "A dense part of the Milky Way galaxy", "Summer",
	imaged_objects.contains("M24")},

        {25, "M25", "", MessierObjectType::OPEN_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.529167, -19.113333, "M25", "Contains about 30 stars visible with binoculars"),
	20.f, 2.f, QSizeF(32., 32.), "Contains about 30 stars visible with binoculars", "Summer",
	imaged_objects.contains("M25")},

        {26, "M26", "", MessierObjectType::OPEN_CLUSTER, Constellation::SCUTUM,
	createSkyPosition(18.754444, -9.386667, "M26", "A relatively sparse open cluster in Scutum"),
	8.9f, 5.f, QSizeF(15., 15.), "A relatively sparse open cluster in Scutum", "Summer",
	imaged_objects.contains("M26")},

        {27, "M27", "Dumbbell Nebula", MessierObjectType::PLANETARY_NEBULA, Constellation::VULPECULA,
	createSkyPosition(19.993434, 22.721198, "M27", "One of the brightest planetary nebulae in the sky"),
	14.1f, 1.2f, QSizeF(8., 5.7), "One of the brightest planetary nebulae in the sky", "Summer",
	imaged_objects.contains("M27")},

        {28, "M28", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.409136, -24.869833, "M28", "Located in the constellation Sagittarius"),
	20.f, 18.6f, QSizeF(11.2, 11.2), "Located in the constellation Sagittarius", "Summer",
	imaged_objects.contains("M28")},

        {29, "M29", "", MessierObjectType::OPEN_CLUSTER, Constellation::CYGNUS,
	createSkyPosition(20.396111, 38.486667, "M29", "A small but bright cluster in Cygnus"),
	6.6f, 4.f, QSizeF(7., 7.), "A small but bright cluster in Cygnus", "Summer",
	imaged_objects.contains("M29")},

        {30, "M30", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::CAPRICORNUS,
	createSkyPosition(21.672811, -23.179861, "M30", "A dense, compact globular cluster"),
	7.1f, 26.1f, QSizeF(11., 11.), "A dense, compact globular cluster", "Autumn",
	imaged_objects.contains("M30")},

        {31, "M31", "Andromeda Galaxy", MessierObjectType::GALAXY, Constellation::ANDROMEDA,
	createSkyPosition(0.712314, 41.26875, "M31", "The nearest major galaxy to the Milky Way"),
	3.4f, 2500.f, QSizeF(178., 63.), "The nearest major galaxy to the Milky Way", "Autumn",
	imaged_objects.contains("M31")},

        {32, "M32", "", MessierObjectType::GALAXY, Constellation::ANDROMEDA,
	createSkyPosition(0.711618, 40.865169, "M32", "A satellite galaxy of the Andromeda Galaxy"),
	8.1f, 2900.f, QSizeF(8.7, 6.5), "A satellite galaxy of the Andromeda Galaxy", "Autumn",
	imaged_objects.contains("M32")},

        {33, "M33", "Triangulum Galaxy", MessierObjectType::GALAXY, Constellation::TRIANGULUM,
	createSkyPosition(1.564138, 30.660175, "M33", "The third-largest galaxy in the Local Group"),
	5.7f, 2900.f, QSizeF(73., 45.), "The third-largest galaxy in the Local Group", "Autumn",
	imaged_objects.contains("M33")},

        {34, "M34", "", MessierObjectType::OPEN_CLUSTER, Constellation::PERSEUS,
	createSkyPosition(2.701944, 42.721667, "M34", "Contains about 100 stars and spans 35 light years"),
	20.f, 1.4f, QSizeF(35., 35.), "Contains about 100 stars and spans 35 light years", "Autumn",
	imaged_objects.contains("M34")},

        {35, "M35", "", MessierObjectType::OPEN_CLUSTER, Constellation::GEMINI,
	createSkyPosition(6.151389, 24.336667, "M35", "A large open cluster visible to the naked eye"),
	20.f, 2.8f, QSizeF(28., 28.), "A large open cluster visible to the naked eye", "Winter",
	imaged_objects.contains("M35")},

        {36, "M36", "", MessierObjectType::OPEN_CLUSTER, Constellation::AURIGA,
	createSkyPosition(5.605556, 34.135, "M36", "A young open cluster in Auriga"),
	6.f, 4.1f, QSizeF(12., 12.), "A young open cluster in Auriga", "Winter",
	imaged_objects.contains("M36")},

        {37, "M37", "", MessierObjectType::OPEN_CLUSTER, Constellation::AURIGA,
	createSkyPosition(5.871667, 32.545, "M37", "The richest open cluster in Auriga"),
	5.6f, 4.5f, QSizeF(24., 24.), "The richest open cluster in Auriga", "Winter",
	imaged_objects.contains("M37")},

        {38, "M38", "", MessierObjectType::OPEN_CLUSTER, Constellation::AURIGA,
	createSkyPosition(5.477778, 35.823333, "M38", "Contains a distinctive cruciform pattern of stars"),
	6.4f, 4.2f, QSizeF(21., 21.), "Contains a distinctive cruciform pattern of stars", "Winter",
	imaged_objects.contains("M38")},

        {39, "M39", "", MessierObjectType::OPEN_CLUSTER, Constellation::CYGNUS,
	createSkyPosition(21.525833, 48.246667, "M39", "A loose, scattered open cluster in Cygnus"),
	20.f, 0.8f, QSizeF(32., 32.), "A loose, scattered open cluster in Cygnus", "Autumn",
	imaged_objects.contains("M39")},

        {40, "M40", "", MessierObjectType::DOUBLE_STAR, Constellation::URSA_MAJOR,
	createSkyPosition(12.37, 58.083333, "M40", "Actually a double star system, not a deep sky object"),
	20.f, 0.5f, QSizeF(0.8, 0.8), "Actually a double star system, not a deep sky object", "Spring",
	imaged_objects.contains("M40")},

        {41, "M41", "", MessierObjectType::OPEN_CLUSTER, Constellation::CANIS_MAJOR,
	createSkyPosition(6.766667, -20.716667, "M41", "A bright open cluster easily visible with binoculars"),
	4.5f, 2.3f, QSizeF(38., 38.), "A bright open cluster easily visible with binoculars", "Winter",
	imaged_objects.contains("M41")},

        {42, "M42", "Orion Nebula", MessierObjectType::NEBULA, Constellation::ORION,
	createSkyPosition(5.588139, -5.391111, "M42", "One of the brightest nebulae visible to the naked eye"),
	20.f, 1.3f, QSizeF(85., 60.), "One of the brightest nebulae visible to the naked eye", "Winter",
	imaged_objects.contains("M42")},

        {43, "M43", "", MessierObjectType::NEBULA, Constellation::ORION,
	createSkyPosition(5.591944, -5.27, "M43", "Part of the Orion Nebula complex"),
	20.f, 1.6f, QSizeF(20., 15.), "Part of the Orion Nebula complex", "Winter",
	imaged_objects.contains("M43")},

        {44, "M44", "Beehive Cluster", MessierObjectType::OPEN_CLUSTER, Constellation::CANCER,
	createSkyPosition(8.670278, 19.621667, "M44", "Also known as Praesepe, visible to naked eye"),
	20.f, 0.6f, QSizeF(95., 95.), "Also known as Praesepe, visible to naked eye", "Winter",
	imaged_objects.contains("M44")},

        {45, "M45", "Pleiades", MessierObjectType::OPEN_CLUSTER, Constellation::TAURUS,
	createSkyPosition(3.773333, 24.113333, "M45", "The Seven Sisters, visible to naked eye"),
	20.f, 0.4f, QSizeF(110., 110.), "The Seven Sisters, visible to naked eye", "Winter",
	imaged_objects.contains("M45")},

        {46, "M46", "", MessierObjectType::OPEN_CLUSTER, Constellation::PUPPIS,
	createSkyPosition(7.696389, -14.843333, "M46", "Contains a planetary nebula within the cluster"),
	20.f, 5.4f, QSizeF(27., 27.), "Contains a planetary nebula within the cluster", "Winter",
	imaged_objects.contains("M46")},

        {47, "M47", "", MessierObjectType::OPEN_CLUSTER, Constellation::PUPPIS,
	createSkyPosition(7.609722, -14.488333, "M47", "A bright, large open cluster in Puppis"),
	20.f, 1.6f, QSizeF(30., 30.), "A bright, large open cluster in Puppis", "Winter",
	imaged_objects.contains("M47")},

        {48, "M48", "", MessierObjectType::OPEN_CLUSTER, Constellation::HYDRA,
	createSkyPosition(8.2275, -5.726667, "M48", "A large open cluster visible with binoculars"),
	20.f, 1.5f, QSizeF(54., 54.), "A large open cluster visible with binoculars", "Winter",
	imaged_objects.contains("M48")},

        {49, "M49", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.496333, 8.000411, "M49", "An elliptical galaxy in the Virgo Cluster"),
	12.2f, 56000.f, QSizeF(9., 7.5), "An elliptical galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M49")},

        {50, "M50", "", MessierObjectType::OPEN_CLUSTER, Constellation::MONOCEROS,
	createSkyPosition(7.046528, -8.337778, "M50", "Contains about 200 stars in a heart-shaped pattern"),
	20.f, 3.f, QSizeF(16., 16.), "Contains about 200 stars in a heart-shaped pattern", "Winter",
	imaged_objects.contains("M50")},

        {51, "M51", "Whirlpool Galaxy", MessierObjectType::GALAXY, Constellation::CANES_VENATICI,
	createSkyPosition(13.497972, 47.195258, "M51", "A classic example of a spiral galaxy"),
	8.4f, 23000.f, QSizeF(11.2, 6.9), "A classic example of a spiral galaxy", "Spring",
	imaged_objects.contains("M51")},

        {52, "M52", "", MessierObjectType::OPEN_CLUSTER, Constellation::CASSIOPEIA,
	createSkyPosition(23.413056, 61.59, "M52", "A rich open cluster in Cassiopeia"),
	20.f, 5.f, QSizeF(13., 13.), "A rich open cluster in Cassiopeia", "Autumn",
	imaged_objects.contains("M52")},

        {53, "M53", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::COMA_BERENICES,
	createSkyPosition(13.215347, 18.168167, "M53", "A globular cluster in the constellation Coma Berenices"),
	7.8f, 58.f, QSizeF(13., 13.), "A globular cluster in the constellation Coma Berenices", "Spring",
	imaged_objects.contains("M53")},

        {54, "M54", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.917592, -30.479861, "M54", "A small, dense globular cluster in Sagittarius"),
	20.f, 87.4f, QSizeF(9.1, 9.1), "A small, dense globular cluster in Sagittarius", "Summer",
	imaged_objects.contains("M54")},

        {55, "M55", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(19.666586, -30.96475, "M55", "A large, bright globular cluster"),
	6.5f, 17.3f, QSizeF(19., 19.), "A large, bright globular cluster", "Summer",
	imaged_objects.contains("M55")},

        {56, "M56", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::LYRA,
	createSkyPosition(19.276547, 30.183472, "M56", "A moderately concentrated globular cluster"),
	20.f, 32.9f, QSizeF(7.1, 7.1), "A moderately concentrated globular cluster", "Summer",
	imaged_objects.contains("M56")},

        {57, "M57", "Ring Nebula", MessierObjectType::PLANETARY_NEBULA, Constellation::LYRA,
	createSkyPosition(18.893082, 33.029134, "M57", "A classic planetary nebula with a ring-like appearance"),
	15.8f, 2.3f, QSizeF(1.4, 1.), "A classic planetary nebula with a ring-like appearance", "Summer",
	imaged_objects.contains("M57")},

        {58, "M58", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.628777, 11.818089, "M58", "A barred spiral galaxy in the Virgo Cluster"),
	9.7f, 62.f, QSizeF(5.9, 4.7), "A barred spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M58")},

        {59, "M59", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.700627, 11.646919, "M59", "An elliptical galaxy in the Virgo Cluster"),
	20.f, 60.f, QSizeF(5.4, 3.7), "An elliptical galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M59")},

        {60, "M60", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.72777, 11.552691, "M60", "A large elliptical galaxy interacting with NGC 4647"),
	20.f, 55.f, QSizeF(7.6, 6.2), "A large elliptical galaxy interacting with NGC 4647", "Spring",
	imaged_objects.contains("M60")},

        {61, "M61", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.365258, 4.473777, "M61", "A spiral galaxy in the Virgo Cluster"),
	9.7f, 52.5f, QSizeF(6.5, 5.9), "A spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M61")},

        {62, "M62", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(17.020167, -30.112361, "M62", "A compact globular cluster near the galactic center"),
	7.4f, 22.5f, QSizeF(15., 15.), "A compact globular cluster near the galactic center", "Summer",
	imaged_objects.contains("M62")},

        {63, "M63", "Sunflower Galaxy", MessierObjectType::GALAXY, Constellation::CANES_VENATICI,
	createSkyPosition(13.263687, 42.029369, "M63", "A spiral galaxy with well-defined arms"),
	8.6f, 37.f, QSizeF(12.6, 7.2), "A spiral galaxy with well-defined arms", "Spring",
	imaged_objects.contains("M63")},

        {64, "M64", "Black Eye Galaxy", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.945471, 21.682658, "M64", "Has a dark band of dust in front of its nucleus"),
	8.5f, 24.f, QSizeF(9.3, 5.4), "Has a dark band of dust in front of its nucleus", "Spring",
	imaged_objects.contains("M64")},

        {65, "M65", "", MessierObjectType::GALAXY, Constellation::LEO,
	createSkyPosition(11.31553, 13.092306, "M65", "Member of the Leo Triplet group of galaxies"),
	20.f, 35.f, QSizeF(9.8, 2.9), "Member of the Leo Triplet group of galaxies", "Spring",
	imaged_objects.contains("M65")},

        {66, "M66", "", MessierObjectType::GALAXY, Constellation::LEO,
	createSkyPosition(11.337507, 12.991289, "M66", "Member of the Leo Triplet group of galaxies"),
	8.9f, 35.f, QSizeF(9.1, 4.2), "Member of the Leo Triplet group of galaxies", "Spring",
	imaged_objects.contains("M66")},

        {67, "M67", "", MessierObjectType::OPEN_CLUSTER, Constellation::CANCER,
	createSkyPosition(8.856389, 11.813333, "M67", "One of the oldest known open clusters"),
	20.f, 2.7f, QSizeF(30., 30.), "One of the oldest known open clusters", "Winter",
	imaged_objects.contains("M67")},

        {68, "M68", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::HYDRA,
	createSkyPosition(12.657772, -26.744056, "M68", "A globular cluster in the constellation Hydra"),
	8.f, 33.6f, QSizeF(12., 12.), "A globular cluster in the constellation Hydra", "Spring",
	imaged_objects.contains("M68")},

        {69, "M69", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.523083, -32.348083, "M69", "A globular cluster near the galactic center"),
	8.3f, 29.7f, QSizeF(7.1, 7.1), "A globular cluster near the galactic center", "Summer",
	imaged_objects.contains("M69")},

        {70, "M70", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(18.720211, -32.292111, "M70", "A compact globular cluster in Sagittarius"),
	9.1f, 29.4f, QSizeF(7.8, 7.8), "A compact globular cluster in Sagittarius", "Summer",
	imaged_objects.contains("M70")},

        {71, "M71", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTA,
	createSkyPosition(19.896247, 18.779194, "M71", "A loose globular cluster, once considered an open cluster"),
	6.1f, 13.f, QSizeF(7.2, 7.2), "A loose globular cluster, once considered an open cluster", "Summer",
	imaged_objects.contains("M71")},

        {72, "M72", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::AQUARIUS,
	createSkyPosition(20.891028, -12.537306, "M72", "A fairly dim and distant globular cluster"),
	9.f, 53.4f, QSizeF(6.6, 6.6), "A fairly dim and distant globular cluster", "Summer",
	imaged_objects.contains("M72")},

        {73, "M73", "", MessierObjectType::ASTERISM, Constellation::AQUARIUS,
	createSkyPosition(20.983333, -12.633333, "M73", "A group of four stars, not a true deep sky object"),
	8.9f, 2.f, QSizeF(2.5, 2.5), "A group of four stars, not a true deep sky object", "Summer",
	imaged_objects.contains("M73")},

        {74, "M74", "", MessierObjectType::GALAXY, Constellation::PISCES,
	createSkyPosition(1.611596, 15.783641, "M74", "A face-on spiral galaxy with well-defined arms"),
	9.5f, 32.f, QSizeF(10.2, 9.5), "A face-on spiral galaxy with well-defined arms", "Autumn",
	imaged_objects.contains("M74")},

        {75, "M75", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SAGITTARIUS,
	createSkyPosition(20.101345, -21.922261, "M75", "A compact, dense globular cluster"),
	8.3f, 67.5f, QSizeF(6.8, 6.8), "A compact, dense globular cluster", "Summer",
	imaged_objects.contains("M75")},

        {76, "M76", "Little Dumbbell Nebula", MessierObjectType::PLANETARY_NEBULA, Constellation::PERSEUS,
	createSkyPosition(1.70546, 51.575426, "M76", "A small, faint planetary nebula"),
	17.5f, 3.4f, QSizeF(2.7, 1.8), "A small, faint planetary nebula", "Autumn",
	imaged_objects.contains("M76")},

        {77, "M77", "", MessierObjectType::GALAXY, Constellation::CETUS,
	createSkyPosition(2.711308, -0.013294, "M77", "A barred spiral galaxy and Seyfert galaxy"),
	8.9f, 47.f, QSizeF(7.1, 6.), "A barred spiral galaxy and Seyfert galaxy", "Autumn",
	imaged_objects.contains("M77")},

        {78, "M78", "", MessierObjectType::NEBULA, Constellation::ORION,
	createSkyPosition(5.779389, 0.079167, "M78", "A reflection nebula in the constellation Orion"),
	20.f, 1.6f, QSizeF(8., 6.), "A reflection nebula in the constellation Orion", "Winter",
	imaged_objects.contains("M78")},

        {79, "M79", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::LEPUS,
	createSkyPosition(5.402942, -24.52425, "M79", "An unusual globular cluster that may have originated outside our galaxy"),
	8.2f, 42.1f, QSizeF(8.7, 8.7), "An unusual globular cluster that may have originated outside our galaxy", "Winter",
	imaged_objects.contains("M79")},

        {80, "M80", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::SCORPIUS,
	createSkyPosition(16.284003, -22.976083, "M80", "A dense, compact globular cluster"),
	20.f, 32.6f, QSizeF(10., 10.), "A dense, compact globular cluster", "Summer",
	imaged_objects.contains("M80")},

        {81, "M81", "Bode's Galaxy", MessierObjectType::GALAXY, Constellation::URSA_MAJOR,
	createSkyPosition(9.925881, 69.065295, "M81", "A grand design spiral galaxy"),
	6.9f, 11.8f, QSizeF(26.9, 14.1), "A grand design spiral galaxy", "Spring",
	imaged_objects.contains("M81")},

        {82, "M82", "Cigar Galaxy", MessierObjectType::GALAXY, Constellation::URSA_MAJOR,
	createSkyPosition(9.931231, 69.679703, "M82", "A starburst galaxy with intense star formation"),
	8.4f, 12.f, QSizeF(11.2, 4.3), "A starburst galaxy with intense star formation", "Spring",
	imaged_objects.contains("M82")},

        {83, "M83", "Southern Pinwheel Galaxy", MessierObjectType::GALAXY, Constellation::HYDRA,
	createSkyPosition(13.616922, -29.865761, "M83", "A face-on spiral galaxy visible from southern hemisphere"),
	7.5f, 15.f, QSizeF(12.9, 11.5), "A face-on spiral galaxy visible from southern hemisphere", "Spring",
	imaged_objects.contains("M83")},

        {84, "M84", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.417706, 12.886983, "M84", "A lenticular galaxy in the Virgo Cluster"),
	10.5f, 60.f, QSizeF(6.5, 5.6), "A lenticular galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M84")},

        {85, "M85", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.423348, 18.191081, "M85", "A lenticular galaxy in the Virgo Cluster"),
	20.f, 60.f, QSizeF(7.1, 5.2), "A lenticular galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M85")},

        {86, "M86", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.436615, 12.945969, "M86", "A lenticular galaxy in the Virgo Cluster"),
	8.9f, 52.f, QSizeF(8.9, 5.8), "A lenticular galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M86")},

        {87, "M87", "Virgo A", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.513729, 12.391123, "M87", "A supergiant elliptical galaxy with active nucleus"),
	8.6f, 53.5f, QSizeF(8.3, 6.6), "A supergiant elliptical galaxy with active nucleus", "Spring",
	imaged_objects.contains("M87")},

        {88, "M88", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.533098, 14.420319, "M88", "A spiral galaxy in the Virgo Cluster"),
	13.2f, 60.f, QSizeF(6.9, 3.7), "A spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M88")},

        {89, "M89", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.594391, 12.556342, "M89", "An elliptical galaxy in the Virgo Cluster"),
	9.8f, 60.f, QSizeF(5.1, 4.2), "An elliptical galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M89")},

        {90, "M90", "", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.613834, 13.162923, "M90", "A spiral galaxy in the Virgo Cluster"),
	9.5f, 60.f, QSizeF(9.5, 4.4), "A spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M90")},

        {91, "M91", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.590679, 14.496322, "M91", "A barred spiral galaxy in the Virgo Cluster"),
	13.6f, 63.f, QSizeF(5.4, 4.4), "A barred spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M91")},

        {92, "M92", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::HERCULES,
	createSkyPosition(17.285386, 43.135944, "M92", "A bright globular cluster in Hercules"),
	6.5f, 26.7f, QSizeF(14., 14.), "A bright globular cluster in Hercules", "Summer",
	imaged_objects.contains("M92")},

        {93, "M93", "", MessierObjectType::OPEN_CLUSTER, Constellation::PUPPIS,
	createSkyPosition(7.742778, -23.853333, "M93", "A bright open cluster with about 80 stars"),
	20.f, 3.6f, QSizeF(22., 22.), "A bright open cluster with about 80 stars", "Winter",
	imaged_objects.contains("M93")},

        {94, "M94", "", MessierObjectType::GALAXY, Constellation::CANES_VENATICI,
	createSkyPosition(12.848076, 41.12025, "M94", "A spiral galaxy with a bright central region"),
	8.2f, 16.f, QSizeF(11.2, 9.1), "A spiral galaxy with a bright central region", "Spring",
	imaged_objects.contains("M94")},

        {95, "M95", "", MessierObjectType::GALAXY, Constellation::LEO,
	createSkyPosition(10.732703, 11.703695, "M95", "A barred spiral galaxy in the Leo I group"),
	9.7f, 38.f, QSizeF(7.4, 5.), "A barred spiral galaxy in the Leo I group", "Spring",
	imaged_objects.contains("M95")},

        {96, "M96", "", MessierObjectType::GALAXY, Constellation::LEO,
	createSkyPosition(10.779373, 11.819939, "M96", "A spiral galaxy in the Leo I group"),
	9.2f, 31.f, QSizeF(7.6, 5.2), "A spiral galaxy in the Leo I group", "Spring",
	imaged_objects.contains("M96")},

        {97, "M97", "Owl Nebula", MessierObjectType::PLANETARY_NEBULA, Constellation::URSA_MAJOR,
	createSkyPosition(11.246587, 55.019023, "M97", "A planetary nebula that resembles an owl's face"),
	15.8f, 2.f, QSizeF(3.4, 3.3), "A planetary nebula that resembles an owl's face", "Spring",
	imaged_objects.contains("M97")},

        {98, "M98", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.230081, 14.900543, "M98", "A spiral galaxy in the Virgo Cluster"),
	10.1f, 60.f, QSizeF(9.8, 2.8), "A spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M98")},

        {99, "M99", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.313785, 14.416489, "M99", "A nearly face-on spiral galaxy in the Virgo Cluster"),
	9.9f, 60.f, QSizeF(5.4, 4.8), "A nearly face-on spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M99")},

        {100, "M100", "", MessierObjectType::GALAXY, Constellation::COMA_BERENICES,
	createSkyPosition(12.381925, 15.822305, "M100", "A grand design spiral galaxy in the Virgo Cluster"),
	9.3f, 55.f, QSizeF(7.4, 6.3), "A grand design spiral galaxy in the Virgo Cluster", "Spring",
	imaged_objects.contains("M100")},

        {101, "M101", "Pinwheel Galaxy", MessierObjectType::GALAXY, Constellation::URSA_MAJOR,
	createSkyPosition(14.053495, 54.34875, "M101", "A face-on spiral galaxy with prominent arms"),
	7.9f, 27.f, QSizeF(28.8, 26.9), "A face-on spiral galaxy with prominent arms", "Spring",
	imaged_objects.contains("M101")},

        {102, "M102", "", MessierObjectType::GALAXY, Constellation::DRACO,
	createSkyPosition(15.108211, 55.763308, "M102", "A lenticular or spiral galaxy in Draco"),
	9.9f, 30.f, QSizeF(5.2, 2.3), "A lenticular or spiral galaxy in Draco", "Summer",
	imaged_objects.contains("M102")},

        {103, "M103", "", MessierObjectType::OPEN_CLUSTER, Constellation::CASSIOPEIA,
	createSkyPosition(1.555833, 60.658333, "M103", "A relatively young open cluster in Cassiopeia"),
	7.4f, 8.5f, QSizeF(6., 6.), "A relatively young open cluster in Cassiopeia", "Autumn",
	imaged_objects.contains("M103")},

        {104, "M104", "Sombrero Galaxy", MessierObjectType::GALAXY, Constellation::VIRGO,
	createSkyPosition(12.666508, -11.623052, "M104", "A galaxy with a distinctive dust lane like a sombrero"),
	8.f, 29.3f, QSizeF(8.7, 3.5), "A galaxy with a distinctive dust lane like a sombrero", "Spring",
	imaged_objects.contains("M104")},

        {105, "M105", "", MessierObjectType::GALAXY, Constellation::LEO,
	createSkyPosition(10.797111, 12.581631, "M105", "An elliptical galaxy in the Leo I group"),
	9.8f, 32.f, QSizeF(5.4, 4.8), "An elliptical galaxy in the Leo I group", "Spring",
	imaged_objects.contains("M105")},

        {106, "M106", "", MessierObjectType::GALAXY, Constellation::CANES_VENATICI,
	createSkyPosition(12.316006, 47.303719, "M106", "A spiral galaxy with an active galactic nucleus"),
	8.4f, 22.8f, QSizeF(18.6, 7.6), "A spiral galaxy with an active galactic nucleus", "Spring",
	imaged_objects.contains("M106")},

        {107, "M107", "", MessierObjectType::GLOBULAR_CLUSTER, Constellation::OPHIUCHUS,
	createSkyPosition(16.542183, -13.053778, "M107", "A globular cluster in Ophiuchus"),
	8.8f, 20.9f, QSizeF(13., 13.), "A globular cluster in Ophiuchus", "Summer",
	imaged_objects.contains("M107")},

        {108, "M108", "", MessierObjectType::GALAXY, Constellation::URSA_MAJOR,
	createSkyPosition(11.191935, 55.674122, "M108", "An edge-on barred spiral galaxy near the Big Dipper"),
	20.f, 45.f, QSizeF(8.7, 2.2), "An edge-on barred spiral galaxy near the Big Dipper", "Spring",
	imaged_objects.contains("M108")},

        {109, "M109", "", MessierObjectType::GALAXY, Constellation::URSA_MAJOR,
	createSkyPosition(11.95999, 53.374724, "M109", "A barred spiral galaxy in Ursa Major"),
	20.f, 55.f, QSizeF(7.6, 4.7), "A barred spiral galaxy in Ursa Major", "Spring",
	imaged_objects.contains("M109")},

        {110, "M110", "", MessierObjectType::GALAXY, Constellation::ANDROMEDA,
	createSkyPosition(0.672794, 41.685419, "M110", "A satellite galaxy of the Andromeda Galaxy"),
	8.1f, 2.2f, QSizeF(21.9, 11.), "A satellite galaxy of the Andromeda Galaxy", "Autumn",
	imaged_objects.contains("M110")},

    };
}

MessierSkyPosition MessierCatalog::createSkyPosition(double ra_hours, double dec_degrees, 
                                            const QString& name, const QString& description) {
    MessierSkyPosition pos;
    pos.ra_deg = raHoursToDegrees(ra_hours);  // Convert RA hours to degrees
    pos.dec_deg = dec_degrees;
    pos.name = name;
    pos.description = description;
    return pos;
}

QList<MessierObject> MessierCatalog::getAllObjects() {
    if (m_catalog.isEmpty()) {
        initializeCatalog();
    }
    return m_catalog;
}

MessierObject MessierCatalog::getObjectById(int id) {
    auto objects = getAllObjects();
    for (const auto& obj : objects) {
        if (obj.id == id) {
            return obj;
        }
    }
    // Return empty object if not found
    return MessierObject{};
}

QList<MessierObject> MessierCatalog::getImagedObjects() {
    auto objects = getAllObjects();
    QList<MessierObject> imaged;
    for (const auto& obj : objects) {
        if (obj.has_been_imaged) {
            imaged.append(obj);
        }
    }
    return imaged;
}

QStringList MessierCatalog::getObjectNames() {
    auto objects = getAllObjects();
    QStringList names;
    for (const auto& obj : objects) {
        QString displayName = obj.name;
        if (!obj.common_name.isEmpty()) {
            displayName += " (" + obj.common_name + ")";
        }
        names.append(displayName);
    }
    return names;
}

QString MessierCatalog::objectTypeToString(MessierObjectType type) {
    switch(type) {
        case MessierObjectType::GLOBULAR_CLUSTER: return "Globular Cluster";
        case MessierObjectType::OPEN_CLUSTER: return "Open Cluster";
        case MessierObjectType::NEBULA: return "Nebula";
        case MessierObjectType::PLANETARY_NEBULA: return "Planetary Nebula";
        case MessierObjectType::SUPERNOVA_REMNANT: return "Supernova Remnant";
        case MessierObjectType::GALAXY: return "Galaxy";
        case MessierObjectType::GALAXY_CLUSTER: return "Galaxy Cluster";
        case MessierObjectType::DOUBLE_STAR: return "Double Star";
        case MessierObjectType::ASTERISM: return "Asterism";
        case MessierObjectType::STAR_CLOUD: return "Star Cloud";
        case MessierObjectType::OTHER: return "Other";
        default: return "Unknown";
    }
}

#endif // MESSIERCATALOG_H

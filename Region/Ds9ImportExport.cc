//# Ds9ImportExport.cc: import and export regions in DS9 format

#include "Ds9ImportExport.h"

#include <casacore/casa/Quanta/QMath.h>
#include <casacore/coordinates/Coordinates/DirectionCoordinate.h>

#include <iomanip>

#include "../Util.h"

using namespace carta;

Ds9ImportExport::Ds9ImportExport(casacore::CoordinateSystem* image_coord_sys, const casacore::IPosition& image_shape, int file_id,
    const std::string& file, bool file_is_filename)
    : RegionImportExport(image_coord_sys, image_shape, file_id), _file_ref_frame("physical"), _pixel_coord(true) {
    // Import regions in DS9 format
    SetParserDelim(" ,()#");
    std::vector<std::string> lines = ReadRegionFile(file, file_is_filename, ';');
    ProcessFileLines(lines);
}

Ds9ImportExport::Ds9ImportExport(casacore::CoordinateSystem* image_coord_sys, const casacore::IPosition& image_shape, bool pixel_coord)
    : RegionImportExport(image_coord_sys, image_shape), _pixel_coord(pixel_coord) {
    // Export regions to DS9 format
    // Set coordinate system for file header
    if (pixel_coord) {
        _file_ref_frame = "physical";
    } else {
        SetImageReferenceFrame();
        // Convert from casacore to ds9 for export file
        InitDs9CoordMap();
        for (auto& coord : _coord_map) {
            if (coord.second == _image_ref_frame) {
                _file_ref_frame = coord.first;
                break;
            }
        }
        // Multiple DS9 options for these frames, force fk*
        if (_image_ref_frame == "B1950") {
            _file_ref_frame = "fk4";
        } else if (_image_ref_frame == "J2000") {
            _file_ref_frame = "fk5";
        }
    }

    AddHeader();
}

Ds9ImportExport::~Ds9ImportExport() {
    delete _coord_sys;
}

// Public: for exporting regions

bool Ds9ImportExport::AddExportRegion(const RegionState& region_state) {
    // Add pixel-coord region using RegionState
    std::vector<CARTA::Point> points = region_state.control_points;
    float angle = region_state.rotation;
    if (region_state.type == CARTA::RegionType::ELLIPSE) {
        angle += 90.0; // DS9 angle measured from x-axis
        if (angle > 360.0) {
            angle -= 360.0;
        }
    }

    std::string region_line;
    switch (region_state.type) {
        case CARTA::RegionType::POINT: {
            // point(x, y)
            region_line = fmt::format("point({:.2f}, {:.2f})", points[0].x(), points[0].y());
            break;
        }
        case CARTA::RegionType::RECTANGLE: {
            // box(x,y,width,height,angle)
            region_line =
                fmt::format("box({:.2f}, {:.2f}, {:.2f}, {:.2f}, {})", points[0].x(), points[0].y(), points[1].x(), points[1].y(), angle);
            break;
        }
        case CARTA::RegionType::ELLIPSE: {
            // ellipse(x,y,radius,radius,angle) OR circle(x,y,radius)
            if (points[1].x() == points[1].y()) { // bmaj == bmin
                region_line = fmt::format("circle({:.2f}, {:.2f}, {:.2f})", points[0].x(), points[0].y(), points[1].x());
            } else {
                if (angle > 0.0) {
                    region_line = fmt::format(
                        "ellipse({:.2f}, {:.2f}, {:.2f}, {:.2f}, {})", points[0].x(), points[0].y(), points[1].x(), points[1].y(), angle);
                } else {
                    region_line =
                        fmt::format("ellipse({:.2f}, {:.2f}, {:.2f}, {:.2f})", points[0].x(), points[0].y(), points[1].x(), points[1].y());
                }
            }
            break;
        }
        case CARTA::RegionType::POLYGON: {
            // polygon(x1,y1,x2,y2,x3,y3,...)
            std::ostringstream os; // format varies based on npoints
            os << "polygon(";
            os << std::fixed << std::setprecision(2) << points[0].x() << ", " << points[0].y();
            for (size_t i = 1; i < points.size(); ++i) {
                os << "," << points[i].x() << "," << points[i].y();
            }
            os << ")";
            region_line = os.str();
            break;
        }
        default:
            break;
    }

    // Add region name
    if (!region_state.name.empty()) {
        region_line.append(" # text={" + region_state.name + "}");
    }

    // End line and add to string vector
    if (!region_line.empty()) {
        region_line.append("\n");
        _export_regions.push_back(region_line);
        return true;
    }

    return false;
}

bool Ds9ImportExport::AddExportRegion(const std::string& name, CARTA::RegionType type,
    const std::vector<casacore::Quantity>& control_points, const casacore::Quantity& rotation) {
    // Add region using values from LCRegion Record (pixel or converted to world)

    float angle = rotation.get("deg").getValue(); // from LCRegion "theta" value in radians

    std::string region;
    if (_pixel_coord) {
        region = AddExportRegionPixel(type, control_points, angle);
    } else {
        region = AddExportRegionWorld(type, control_points, angle);
    }

    // Add region name
    if (!name.empty()) {
        region.append(" # text={" + name + "}");
    }

    // Add to string vector
    if (!region.empty()) {
        region.append("\n");
        _export_regions.push_back(region);
        return true;
    }

    return false;
}

bool Ds9ImportExport::ExportRegions(std::string& filename, std::string& error) {
    // Print regions to DS9 file
    if (_export_regions.empty()) {
        error = "Export region failed: no regions to export.";
        return false;
    }

    std::ofstream export_file(filename);
    for (auto& region : _export_regions) {
        export_file << region;
    }
    export_file.close();
    return true;
}

bool Ds9ImportExport::ExportRegions(std::vector<std::string>& contents, std::string& error) {
    // Print regions to DS9 file lines in vector
    if (_export_regions.empty()) {
        error = "Export region failed: no regions to export.";
        return false;
    }

    contents = _export_regions;
    return true;
}

// Process file import
void Ds9ImportExport::ProcessFileLines(std::vector<std::string>& lines) {
    // Process or ignore each file line
    if (lines.empty()) {
        return;
    }

    // Map to check for DS9 keywords and convert to CASA
    InitDs9CoordMap();

    bool ds9_coord_sys_ok(true); // flag for invalid coord sys lines
    for (auto& line : lines) {
        // skip blank line
        if (line.empty()) {
            continue;
        }
        // skip comment
        if (line[0] == '#') {
            continue;
        }
        // skip regions excluded for later analysis (annotation-only)
        if (line[0] == '-') {
            continue;
        }
        // skip global settings not used in carta (yet)
        if (line.find("global") != std::string::npos) {
            continue;
        }

        // process coordinate system; global or for a region definition
        if (IsDs9CoordSysKeyword(line)) {
            // Get ready for conversion
            ds9_coord_sys_ok = SetFileReferenceFrame(line);
            if (!ds9_coord_sys_ok) {
                std::string csys_error = "coord sys " + line + " not supported.\n";
                _import_errors.append(csys_error);
            }
            continue;
        }

        if (ds9_coord_sys_ok) { // else skip lines defined in that coord sys
            SetRegion(line);
        }
    }
}

// Coordinate system handlers

void Ds9ImportExport::InitDs9CoordMap() {
    // for converting coordinate system from DS9 to casacore
    _coord_map["physical"] = "";
    _coord_map["image"] = "";
    _coord_map["b1950"] = "B1950";
    _coord_map["fk4"] = "B1950";
    _coord_map["j2000"] = "J2000";
    _coord_map["fk5"] = "J2000";
    _coord_map["galactic"] = "GALACTIC";
    _coord_map["ecliptic"] = "ECLIPTIC";
    _coord_map["icrs"] = "ICRS";
    _coord_map["wcs"] = "UNSUPPORTED";
    _coord_map["wcsa"] = "UNSUPPORTED";
    _coord_map["linear"] = "UNSUPPORTED";
}

bool Ds9ImportExport::IsDs9CoordSysKeyword(std::string& input_line) {
    // Check if region file line is coordinate in map
    std::string input_lower(input_line);
    std::transform(input_line.begin(), input_line.end(), input_lower.begin(), ::tolower); // convert to lowercase
    return _coord_map.count(input_lower);
}

bool Ds9ImportExport::SetFileReferenceFrame(std::string& ds9_coord) {
    // Convert DS9 coord string in region file to CASA reference frame.
    // Returns whether conversion was successful or undefined/not supported.
    _file_ref_frame = "UNSUPPORTED";

    // Convert in-place to lowercase for map
    std::transform(ds9_coord.begin(), ds9_coord.end(), ds9_coord.begin(), ::tolower);

    // Convert to CASA and reset pixel_coord
    if (_coord_map.count(ds9_coord)) {
        _file_ref_frame = _coord_map[ds9_coord];
        if ((ds9_coord != "physical") && (ds9_coord != "image")) {
            _pixel_coord = false;
            // Set image reference frame for conversion
            if (_image_ref_frame.empty()) {
                SetImageReferenceFrame();
            }
        }
    }

    if (_file_ref_frame == "UNSUPPORTED") {
        _pixel_coord = false;
        return false;
    }

    return true;
}

void Ds9ImportExport::SetImageReferenceFrame() {
    // Set image coord sys direction frame
    if (_coord_sys->hasDirectionCoordinate()) {
        casacore::MDirection::Types reference_frame = _coord_sys->directionCoordinate().directionType();
        _image_ref_frame = casacore::MDirection::showType(reference_frame);
    } else if (_coord_sys->hasLinearCoordinate()) {
        _image_ref_frame = "linear";
    } else {
        _image_ref_frame = "physical";
    }
}

// Import regions into RegionState vector

void Ds9ImportExport::SetRegion(std::string& region_definition) {
    // Convert ds9 region description into RegionState
    // Split into region definition, properties
    std::vector<std::string> parameters;
    std::unordered_map<std::string, std::string> properties;
    ParseRegionParameters(region_definition, parameters, properties);

    // Process region definition include/exclude and remove indicator
    std::string region_type(parameters[0]);
    bool exclude_region(false);
    if (region_type[0] == '+') {
        region_type = region_type.substr(1);
    } else if (region_type[0] == '-') {
        exclude_region = true;
        region_type = region_type.substr(1);
    }

    // For now, we only use the "text" property
    std::string region_name;
    if (properties.count("text")) {
        region_name = properties["text"];
    }

    // Create RegionState based on type
    // Order is important, could be a shaped point e.g. "circle point" is a point not a circle
    if (region_type.find("point") != std::string::npos) {
        ImportPointRegion(parameters, region_name, exclude_region);
    } else if (region_type.find("circle") != std::string::npos) {
        ImportCircleRegion(parameters, region_name, exclude_region);
    } else if (region_type.find("ellipse") != std::string::npos) {
        ImportEllipseRegion(parameters, region_name, exclude_region);
    } else if (region_type.find("box") != std::string::npos) {
        ImportRectangleRegion(parameters, region_name, exclude_region);
    } else if (region_type.find("polygon") != std::string::npos) {
        ImportPolygonRegion(parameters, region_name, exclude_region);
    } else if (region_type.find("line") != std::string::npos) {
        _import_errors.append("DS9 line region not supported.\n");
    } else if (region_type.find("vector") != std::string::npos) {
        _import_errors.append("DS9 vector region not supported.\n");
    } else if (region_type.find("text") != std::string::npos) {
        _import_errors.append("DS9 text not supported.\n");
    } else if (region_type.find("annulus") != std::string::npos) {
        _import_errors.append("DS9 annulus region not supported.\n");
    }
}

void Ds9ImportExport::ImportPointRegion(std::vector<std::string>& parameters, std::string& name, bool exclude_region) {
    // Import DS9 point into CARTA RegionState
    // point x y, circle point x y (various shapes for "circle")
    size_t nparam(parameters.size());
    if ((nparam < 3) || ((parameters[0] != "point") && (parameters[1] != "point"))) {
        std::string syntax_error = "point syntax error.\n";
        _import_errors.append(syntax_error);
        return;
    }

    size_t first_param(1);
    if (parameters[1] == "point") {
        first_param = 2;
    }

    std::vector<casacore::Quantity> param_quantities;
    for (size_t i = first_param; i < nparam; ++i) {
        std::string param(parameters[i]);
        // Convert DS9 unit to Quantity unit for readQuantity
        if (CheckAndConvertParameter(param, "point")) {
            if (i == first_param + 1) {
                ConvertTimeFormatToDeg(param); // ':' to '.'
            }

            // Read string into casacore::Quantity, add to vector
            casacore::Quantity param_quantity;
            if (readQuantity(param_quantity, param)) {
                if (param_quantity.getUnit().empty()) {
                    if (_pixel_coord) {
                        param_quantity.setUnit("pixel");
                    } else {
                        param_quantity.setUnit("deg");
                    }
                }
                param_quantities.push_back(param_quantity);
            } else {
                std::string invalid_param("Invalid point parameter: " + param + ".\n");
                _import_errors.append(invalid_param);
                return;
            }
        } else {
            return;
        }
    }

    // Control points in pixel coordinates
    std::vector<CARTA::Point> control_points;
    if (_pixel_coord) {
        CARTA::Point point;
        point.set_x(param_quantities[0].getValue());
        point.set_y(param_quantities[1].getValue());
        control_points.push_back(point);
    } else {
        casacore::Vector<casacore::Double> pixel_coords;
        if (ConvertPointToPixels(_file_ref_frame, param_quantities, pixel_coords)) {
            CARTA::Point point;
            point.set_x(pixel_coords(0));
            point.set_y(pixel_coords(1));
            control_points.push_back(point);
        } else {
            std::string invalid_param("Failed to apply point to image.\n");
            _import_errors.append(invalid_param);
            return;
        }
    }

    // Create RegionState
    CARTA::RegionType type(CARTA::RegionType::POINT);
    float rotation(0.0);
    RegionState region_state = RegionState(_file_id, name, type, control_points, rotation);
    _import_regions.push_back(region_state);
}

void Ds9ImportExport::ImportCircleRegion(std::vector<std::string>& parameters, std::string& name, bool exclude_region) {
    // Import DS9 circle into CARTA RegionState
    // circle x y radius
    // Convert params to ellipse region (CARTA only has ellipse region) with no angle
    if (parameters.size() >= 4) {
        std::vector<std::string> ellipse_params = {"ellipse", parameters[1], parameters[2], parameters[3], parameters[3]};
        ImportEllipseRegion(ellipse_params, name, exclude_region);
    } else {
        std::string syntax_error = "circle syntax error.\n";
        _import_errors.append(syntax_error);
    }
}

void Ds9ImportExport::ImportEllipseRegion(std::vector<std::string>& parameters, std::string& name, bool exclude_region) {
    // Import DS9 ellipse into CARTA RegionState
    // ellipse x y radius radius [angle]
    size_t nparam(parameters.size());
    if ((nparam == 5) || (nparam == 6)) {
        bool is_circle = (parameters[3] == parameters[4]);
        // convert strings to Quantities
        std::vector<casacore::Quantity> param_quantities;
        std::vector<casacore::String> units = {"", "deg", "deg", "arcsec", "arcsec", "deg"};
        for (size_t i = 1; i < nparam; ++i) {
            std::string param(parameters[i]);
            // Convert DS9 unit to Quantity unit for readQuantity
            if (CheckAndConvertParameter(param, "ellipse")) {
                if (i == 2) {
                    ConvertTimeFormatToDeg(param); // ':' to '.'
                }
                casacore::Quantity param_quantity;
                if (readQuantity(param_quantity, param)) {
                    if (param_quantity.getUnit().empty()) {
                        if ((i == nparam - 1) || !_pixel_coord) {
                            param_quantity.setUnit(units[i]);
                        } else {
                            param_quantity.setUnit("pixel");
                        }
                    }
                    param_quantities.push_back(param_quantity);
                } else {
                    std::string invalid_param("Invalid ellipse parameter " + param + ".\n");
                    _import_errors.append(invalid_param);
                    return;
                }
            } else {
                return;
            }
        }

        // Control points in pixel coordinates
        std::vector<CARTA::Point> control_points;
        if (_pixel_coord) {
            CARTA::Point point;
            point.set_x(param_quantities[0].getValue());
            point.set_y(param_quantities[1].getValue());
            control_points.push_back(point);
            point.set_x(param_quantities[2].getValue());
            point.set_y(param_quantities[3].getValue());
            control_points.push_back(point);
        } else {
            // cx, cy
            std::vector<casacore::Quantity> center_coords;
            center_coords.push_back(param_quantities[0]);
            center_coords.push_back(param_quantities[1]);
            casacore::Vector<casacore::Double> pixel_coords;
            if (ConvertPointToPixels(_file_ref_frame, center_coords, pixel_coords)) {
                CARTA::Point point;
                point.set_x(pixel_coords(0));
                point.set_y(pixel_coords(1));
                control_points.push_back(point);
            } else {
                _import_errors.append("Failed to apply ellipse to image.\n");
                return;
            }

            // bmaj, bmin
            CARTA::Point point;
            point.set_x(WorldToPixelLength(param_quantities[2], 0));
            point.set_y(WorldToPixelLength(param_quantities[3], 1));
            control_points.push_back(point);
        }

        // Create RegionState
        CARTA::RegionType type(CARTA::RegionType::ELLIPSE);
        float rotation(0.0);
        if (nparam > 5) {
            rotation = param_quantities[4].getValue();
        }
        if (!is_circle) {
            rotation -= 90.0;
            if (rotation < 0.0) {
                rotation += 360.0;
            }
        }

        RegionState region_state = RegionState(_file_id, name, type, control_points, rotation);
        _import_regions.push_back(region_state);
    } else if (nparam > 6) {
        // unsupported ellipse annulus: ellipse x y r11 r12 r21 r22 [angle]
        _import_errors.append("Unsupported ellipse definition.\n");
    } else {
        _import_errors.append("ellipse syntax error.\n");
    }
}

void Ds9ImportExport::ImportRectangleRegion(std::vector<std::string>& parameters, std::string& name, bool exclude_region) {
    // Import DS9 box into CARTA RegionState
    // box x y width height [angle]
    size_t nparam(parameters.size());
    if ((nparam == 5) || (nparam == 6)) {
        // convert strings to Quantities
        std::vector<casacore::Quantity> param_quantities;
        // DS9 wcs default units
        std::vector<casacore::String> ds9_units = {"", "deg", "deg", "arcsec", "arcsec", "deg"};
        for (size_t i = 1; i < nparam; ++i) {
            std::string param(parameters[i]);
            // Convert DS9 unit to Quantity unit for readQuantity
            if (CheckAndConvertParameter(param, "box")) {
                if (i == 2) {
                    ConvertTimeFormatToDeg(param); // ':' to '.'
                }
                casacore::Quantity param_quantity;
                if (readQuantity(param_quantity, param)) {
                    if (param_quantity.getUnit().empty()) {
                        if ((i == nparam - 1) || !_pixel_coord) {
                            param_quantity.setUnit(ds9_units[i]);
                        } else {
                            param_quantity.setUnit("pixel");
                        }
                    }
                    param_quantities.push_back(param_quantity);
                } else {
                    std::string invalid_param("Invalid box parameter: " + param + ".\n");
                    _import_errors.append(invalid_param);
                    return;
                }
            } else {
                return;
            }
        }

        // Control points in pixel coordinates
        std::vector<CARTA::Point> control_points;
        if (_pixel_coord) {
            CARTA::Point point;
            point.set_x(param_quantities[0].getValue());
            point.set_y(param_quantities[1].getValue());
            control_points.push_back(point);
            point.set_x(param_quantities[2].getValue());
            point.set_y(param_quantities[3].getValue());
            control_points.push_back(point);
        } else {
            // cx, cy
            std::vector<casacore::Quantity> center_coords;
            center_coords.push_back(param_quantities[0]);
            center_coords.push_back(param_quantities[1]);
            casacore::Vector<casacore::Double> pixel_coords;
            if (ConvertPointToPixels(_file_ref_frame, center_coords, pixel_coords)) {
                CARTA::Point point;
                point.set_x(pixel_coords(0));
                point.set_y(pixel_coords(1));
                control_points.push_back(point);
            } else {
                _import_errors.append("Failed to apply box to image.\n");
                return;
            }

            // width, height
            CARTA::Point point;
            point.set_x(WorldToPixelLength(param_quantities[2], 0));
            point.set_y(WorldToPixelLength(param_quantities[3], 1));
            control_points.push_back(point);
        }

        // Create RegionState
        CARTA::RegionType type(CARTA::RegionType::RECTANGLE);
        float rotation(0.0);
        if (nparam > 5) {
            rotation = param_quantities[4].getValue();
        }
        RegionState region_state = RegionState(_file_id, name, type, control_points, rotation);
        _import_regions.push_back(region_state);
    } else if (nparam > 6) {
        // unsupported box annulus: box x y w1 h1 w2 h2 [angle]
        _import_errors.append("Unsupported box definition.\n");
    } else {
        _import_errors.append("box syntax error.\n");
    }
}

void Ds9ImportExport::ImportPolygonRegion(std::vector<std::string>& parameters, std::string& name, bool exclude_region) {
    // Import DS9 polygon into CARTA RegionState
    // polygon x1 y1 x2 y2 x3 y3 ...
    size_t nparam(parameters.size());
    if ((nparam % 2) != 1) { // parameters[0] is "polygon"
        _import_errors.append("polygon syntax error, odd number of arguments.\n");
        return;
    }

    // convert strings to Quantities
    std::vector<casacore::Quantity> param_quantities;
    for (size_t i = 1; i < nparam; ++i) {
        std::string param(parameters[i]);
        // Convert DS9 unit to Quantity unit for readQuantity
        if (CheckAndConvertParameter(param, "polygon")) {
            if ((i % 2) == 0) {
                ConvertTimeFormatToDeg(param); // ':' to '.'
            }
            casacore::Quantity param_quantity;
            if (readQuantity(param_quantity, param)) {
                if (param_quantity.getUnit().empty()) {
                    if (_pixel_coord) {
                        param_quantity.setUnit("pixel");
                    } else {
                        param_quantity.setUnit("deg");
                    }
                }
                param_quantities.push_back(param_quantity);
            } else {
                std::string invalid_param("Invalid polygon parameter " + param + ".\n");
                _import_errors.append(invalid_param);
                return;
            }
        } else {
            return;
        }
    }

    // Control points in pixel coordinates
    std::vector<CARTA::Point> control_points;
    for (size_t i = 0; i < param_quantities.size(); i += 2) {
        if (_pixel_coord) {
            CARTA::Point point;
            point.set_x(param_quantities[i].getValue());
            point.set_y(param_quantities[i + 1].getValue());
            control_points.push_back(point);
        } else {
            std::vector<casacore::Quantity> point;
            point.push_back(param_quantities[i]);
            point.push_back(param_quantities[i + 1]);
            casacore::Vector<casacore::Double> pixel_coords;
            if (ConvertPointToPixels(_file_ref_frame, point, pixel_coords)) {
                CARTA::Point point;
                point.set_x(pixel_coords(0));
                point.set_y(pixel_coords(1));
                control_points.push_back(point);
            } else {
                _import_errors.append("Failed to apply polygon to image.\n");
                return;
            }
        }
    }

    // Create RegionState
    CARTA::RegionType type(CARTA::RegionType::POLYGON);
    float rotation(0.0);
    RegionState region_state = RegionState(_file_id, name, type, control_points, rotation);
    _import_regions.push_back(region_state);
}

bool Ds9ImportExport::CheckAndConvertParameter(std::string& parameter, const std::string& region_type) {
    // Replace DS9 unit with casacore::Quantity unit in parameter string for readQuantity
    // Returns whether valid ds9 parameter
    bool valid(false);
    std::string error_prefix(region_type + " invalid parameter ");

    // use stod to find index of unit in string (after numeric value)
    size_t idx;
    try {
        double val = stod(parameter, &idx); // string to double
    } catch (std::invalid_argument& err) {
        std::string invalid_arg(error_prefix + parameter + ", not a numeric value.\n");
        _import_errors.append(invalid_arg);
        return valid;
    }

    size_t param_length(parameter.length());
    valid = (param_length == idx); // no unit is valid
    if (!valid) {                  // check unit/format
        if (param_length == (idx + 1)) {
            // DS9 units are a single character
            const char unit = parameter.back();
            std::string casacore_unit;
            if (unit == 'd') {
                casacore_unit = "deg";
                valid = true;
            } else if (unit == 'r') {
                casacore_unit = "rad";
                valid = true;
            } else if (unit == 'p') {
                casacore_unit = "pixel";
                valid = true;
            } else if (unit == 'i') {
                casacore_unit = "pixel";
                valid = true;
            } else if ((unit == '"') || (unit == '\'')) {
                // casacore unit for min, sec is the same
                valid = true;
            } else {
                std::string invalid_unit(error_prefix + "unit: " + parameter + ".\n");
                _import_errors.append(invalid_unit);
                valid = false;
            }

            if (!casacore_unit.empty()) {
                // replace DS9 unit with casacore unit
                parameter.pop_back();
                parameter.append(casacore_unit);
            }
        } else {
            // check for hms, dms formats
            const char* param_carray = parameter.c_str();
            float h, m, s;
            valid = ((sscanf(param_carray, "%f:%f:%f", &h, &m, &s) == 3) || (sscanf(param_carray, "%fh%fm%fs", &h, &m, &s) == 3) ||
                     (sscanf(param_carray, "%fd%fm%fs", &h, &m, &s) == 3));
            if (!valid) {
                // Unit not a single character or time/angle format
                std::string invalid_unit(error_prefix + "unit: " + parameter + ".\n");
                _import_errors.append(invalid_unit);
            }
        }
    }
    return valid;
}

void Ds9ImportExport::ConvertTimeFormatToDeg(std::string& parameter) {
    // If parameter is in sexagesimal format dd:mm::ss.ssss, convert to angle format dd.mm.ss.ssss for readQuantity
    for (std::string::iterator it = parameter.begin(); it != parameter.end(); ++it) {
        if (*it == ':') {
            *it = '.';
        }
    }
}

// For export

void Ds9ImportExport::AddHeader() {
    // print file format, globals, and coord sys
    std::ostringstream os;
    os << "# Region file format: DS9 CARTA " << VERSION_ID << std::endl;
    Ds9Properties globals;
    os << "global color=" << globals.color << " delete=" << globals.delete_region << " edit=" << globals.edit_region
       << " fixed=" << globals.fixed_region << " font=\"" << globals.font << "\" highlite=" << globals.highlite_region
       << " include=" << globals.include_region << " move=" << globals.move_region << " select=" << globals.select_region << "\n";
    std::string header = os.str();
    _export_regions.push_back(header);

    os.str("");
    if (_file_ref_frame.empty()) {
        os << "image\n";
    } else {
        os << _file_ref_frame << "\n";
    }
    std::string csys = os.str();
    _export_regions.push_back(csys);
}

std::string Ds9ImportExport::AddExportRegionPixel(
    CARTA::RegionType type, const std::vector<casacore::Quantity>& control_points, float angle) {
    // Add region using Record (pixel or world)
    std::string region;

    switch (type) {
        case CARTA::RegionType::POINT: {
            // point(x, y)
            region = fmt::format("point({:.4f}, {:.4f})", control_points[0].getValue(), control_points[1].getValue());
            break;
        }
        case CARTA::RegionType::RECTANGLE: {
            // box(x,y,width,height,angle)
            region = fmt::format("box({:.4f}, {:.4f}, {:.4f}, {:.4f}, {})", control_points[0].getValue(), control_points[1].getValue(),
                control_points[2].getValue(), control_points[3].getValue(), angle);
            break;
        }
        case CARTA::RegionType::ELLIPSE: {
            // ellipse(x,y,radius,radius,angle) OR circle(x,y,radius)
            if (control_points[2].getValue() == control_points[3].getValue()) { // bmaj == bmin
                region = fmt::format("circle({:.4f}, {:.4f}, {:.4f}\")", control_points[0].getValue(), control_points[1].getValue(),
                    control_points[2].getValue());
            } else {
                if (angle == 0.0) {
                    region = fmt::format("ellipse({:.4f}, {:.4f}, {:.4f}, {:.4f})", control_points[0].getValue(),
                        control_points[1].getValue(), control_points[2].getValue(), control_points[3].getValue());
                } else {
                    region = fmt::format("ellipse({:.4f}, {:.4f}, {:.4f}, {:.4f}, {})", control_points[0].getValue(),
                        control_points[1].getValue(), control_points[2].getValue(), control_points[3].getValue(), angle);
                }
            }
            break;
        }
        case CARTA::RegionType::POLYGON: {
            // polygon(x1,y1,x2,y2,x3,y3,...)
            std::ostringstream os; // format varies based on npoints
            os << "polygon(";
            os << std::fixed << std::setprecision(4) << control_points[0].getValue() << ", " << control_points[1].getValue();
            for (size_t i = 2; i < control_points.size(); i += 2) {
                os << ", " << control_points[i].getValue() << ", " << control_points[i + 1].getValue();
            }
            os << ")";
            region = os.str();
            break;
        }
        default:
            break;
    }

    return region;
}

std::string Ds9ImportExport::AddExportRegionWorld(
    CARTA::RegionType type, const std::vector<casacore::Quantity>& control_points, float angle) {
    // Add region using Record (world coords)
    std::string region;

    switch (type) {
        case CARTA::RegionType::POINT: {
            // point(x, y)
            if (_file_ref_frame.empty()) { // linear coordinates
                region = fmt::format("point({:.6f}, {:.6f})", control_points[0].getValue(), control_points[1].getValue());
            } else {
                region =
                    fmt::format("point({:.6f}, {:.6f})", control_points[0].get("deg").getValue(), control_points[1].get("deg").getValue());
            }
            break;
        }
        case CARTA::RegionType::RECTANGLE: {
            // box(x,y,width,height,angle)
            casacore::Quantity cx(control_points[0]), cy(control_points[1]);
            casacore::Quantity width(control_points[2]), height(control_points[3]);
            if (_file_ref_frame.empty()) { // linear coordinates
                region = fmt::format("box({:.6f}, {:.6f}, {:.4f}\", {:.4f}\", {})", cx.getValue(), cy.getValue(), width.getValue(),
                    height.getValue(), angle);
            } else {
                region = fmt::format("box({:.6f}, {:.6f}, {:.4f}\", {:.4f}\", {})", cx.get("deg").getValue(), cy.get("deg").getValue(),
                    width.get("arcsec").getValue(), height.get("arcsec").getValue(), angle);
            }
            break;
        }
        case CARTA::RegionType::ELLIPSE: {
            // ellipse(x,y,radius,radius,angle) OR circle(x,y,radius)
            if (control_points[2].getValue() == control_points[3].getValue()) {
                // circle when bmaj == bmin
                if (_file_ref_frame.empty()) { // linear coordinates
                    region = fmt::format("circle({:.6f}, {:.6f}, {:.4f}\")", control_points[0].getValue(), control_points[1].getValue(),
                        control_points[2].getValue());
                } else {
                    region = fmt::format("circle({:.6f}, {:.6f}, {:.4f}\")", control_points[0].get("deg").getValue(),
                        control_points[1].get("deg").getValue(), control_points[2].get("arcsec").getValue());
                }
            } else {
                if (_file_ref_frame.empty()) { // linear coordinates
                    region = fmt::format("ellipse({:.6f}, {:.6f}, {:.4f}\", {:.4f}\", {})", control_points[0].getValue(),
                        control_points[1].getValue(), control_points[2].getValue(), control_points[3].getValue(), angle);
                } else {
                    region = fmt::format("ellipse({:.6f}, {:.6f}, {:.4f}\", {:.4f}\", {})", control_points[0].get("deg").getValue(),
                        control_points[1].get("deg").getValue(), control_points[2].get("arcsec").getValue(),
                        control_points[3].get("arcsec").getValue(), angle);
                }
            }
            break;
        }
        case CARTA::RegionType::POLYGON: {
            // polygon(x1,y1,x2,y2,x3,y3,...)
            std::ostringstream os; // format varies based on npoints
            os << "polygon(";
            if (_file_ref_frame.empty()) { // linear coordinates
                os << std::fixed << std::setprecision(6) << control_points[0].getValue();
                for (size_t i = 1; i < control_points.size(); ++i) {
                    os << "," << std::fixed << std::setprecision(6) << control_points[i].getValue();
                }
            } else {
                os << std::fixed << std::setprecision(6) << control_points[0].get("deg").getValue();
                for (size_t i = 1; i < control_points.size(); ++i) {
                    os << "," << std::fixed << std::setprecision(6) << control_points[i].get("deg").getValue();
                }
            }
            os << ")";
            region = os.str();
            break;
        }
        default:
            break;
    }

    return region;
}
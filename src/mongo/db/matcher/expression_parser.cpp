// expression_parser.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"


namespace {

using namespace mongo;

/**
 * Returns true if subtree contains MatchExpression 'type'.
 */
bool hasNode(const MatchExpression* root, MatchExpression::MatchType type) {
    if (type == root->matchType()) {
        return true;
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        if (hasNode(root->getChild(i), type)) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace mongo {

using std::string;

StatusWithMatchExpression MatchExpressionParser::_parseComparison(const char* name,
                                                                  ComparisonMatchExpression* cmp,
                                                                  const BSONElement& e) {
    std::unique_ptr<ComparisonMatchExpression> temp(cmp);

    // Non-equality comparison match expressions cannot have
    // a regular expression as the argument (e.g. {a: {$gt: /b/}} is illegal).
    if (MatchExpression::EQ != cmp->matchType() && RegEx == e.type()) {
        std::stringstream ss;
        ss << "Can't have RegEx as arg to predicate over field '" << name << "'.";
        return {Status(ErrorCodes::BadValue, ss.str())};
    }

    Status s = temp->init(name, e);
    if (!s.isOK())
        return s;

    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseSubField(const BSONObj& context,
                                                                const AndMatchExpression* andSoFar,
                                                                const char* name,
                                                                const BSONElement& e,
                                                                int level) {
    // TODO: these should move to getGtLtOp, or its replacement

    if (mongoutils::str::equals("$eq", e.fieldName()))
        return _parseComparison(name, new EqualityMatchExpression(), e);

    if (mongoutils::str::equals("$not", e.fieldName())) {
        return _parseNot(name, e, level);
    }

    int x = e.getGtLtOp(-1);
    switch (x) {
        case -1:
            // $where cannot be a sub-expression because it works on top-level documents only.
            if (mongoutils::str::equals("$where", e.fieldName())) {
                return {Status(ErrorCodes::BadValue, "$where cannot be applied to a field")};
            }

            return {Status(ErrorCodes::BadValue,
                           mongoutils::str::stream() << "unknown operator: " << e.fieldName())};
        case BSONObj::LT:
            return _parseComparison(name, new LTMatchExpression(), e);
        case BSONObj::LTE:
            return _parseComparison(name, new LTEMatchExpression(), e);
        case BSONObj::GT:
            return _parseComparison(name, new GTMatchExpression(), e);
        case BSONObj::GTE:
            return _parseComparison(name, new GTEMatchExpression(), e);
        case BSONObj::NE: {
            if (RegEx == e.type()) {
                // Just because $ne can be rewritten as the negation of an
                // equality does not mean that $ne of a regex is allowed. See SERVER-1705.
                return {Status(ErrorCodes::BadValue, "Can't have regex as arg to $ne.")};
            }
            StatusWithMatchExpression s = _parseComparison(name, new EqualityMatchExpression(), e);
            if (!s.isOK())
                return s;
            std::unique_ptr<NotMatchExpression> n = stdx::make_unique<NotMatchExpression>();
            Status s2 = n->init(s.getValue().release());
            if (!s2.isOK())
                return s2;
            return {std::move(n)};
        }
        case BSONObj::Equality:
            return _parseComparison(name, new EqualityMatchExpression(), e);

        case BSONObj::opIN: {
            if (e.type() != Array)
                return {Status(ErrorCodes::BadValue, "$in needs an array")};
            std::unique_ptr<InMatchExpression> temp = stdx::make_unique<InMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            s = _parseArrayFilterEntries(temp->getArrayFilterEntries(), e.Obj());
            if (!s.isOK())
                return s;
            return {std::move(temp)};
        }

        case BSONObj::NIN: {
            if (e.type() != Array)
                return {Status(ErrorCodes::BadValue, "$nin needs an array")};
            std::unique_ptr<InMatchExpression> temp = stdx::make_unique<InMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            s = _parseArrayFilterEntries(temp->getArrayFilterEntries(), e.Obj());
            if (!s.isOK())
                return s;

            std::unique_ptr<NotMatchExpression> temp2 = stdx::make_unique<NotMatchExpression>();
            s = temp2->init(temp.release());
            if (!s.isOK())
                return s;

            return {std::move(temp2)};
        }

        case BSONObj::opSIZE: {
            int size = 0;
            if (e.type() == String) {
                // matching old odd semantics
                size = 0;
            } else if (e.type() == NumberInt || e.type() == NumberLong) {
                if (e.numberLong() < 0) {
                    // SERVER-11952. Setting 'size' to -1 means that no documents
                    // should match this $size expression.
                    size = -1;
                } else {
                    size = e.numberInt();
                }
            } else if (e.type() == NumberDouble) {
                if (e.numberInt() == e.numberDouble()) {
                    size = e.numberInt();
                } else {
                    // old semantcs require exact numeric match
                    // so [1,2] != 1 or 2
                    size = -1;
                }
            } else {
                return {Status(ErrorCodes::BadValue, "$size needs a number")};
            }

            std::unique_ptr<SizeMatchExpression> temp = stdx::make_unique<SizeMatchExpression>();
            Status s = temp->init(name, size);
            if (!s.isOK())
                return s;
            return {std::move(temp)};
        }

        case BSONObj::opEXISTS: {
            if (e.eoo())
                return {Status(ErrorCodes::BadValue, "$exists can't be eoo")};
            std::unique_ptr<ExistsMatchExpression> temp =
                stdx::make_unique<ExistsMatchExpression>();
            Status s = temp->init(name);
            if (!s.isOK())
                return s;
            if (e.trueValue())
                return {std::move(temp)};
            std::unique_ptr<NotMatchExpression> temp2 = stdx::make_unique<NotMatchExpression>();
            s = temp2->init(temp.release());
            if (!s.isOK())
                return s;
            return {std::move(temp2)};
        }

        case BSONObj::opTYPE:
            return _parseType(name, e);

        case BSONObj::opMOD:
            return _parseMOD(name, e);

        case BSONObj::opOPTIONS: {
            // TODO: try to optimize this
            // we have to do this since $options can be before or after a $regex
            // but we validate here
            BSONObjIterator i(context);
            while (i.more()) {
                BSONElement temp = i.next();
                if (temp.getGtLtOp(-1) == BSONObj::opREGEX)
                    return {nullptr};
            }

            return {Status(ErrorCodes::BadValue, "$options needs a $regex")};
        }

        case BSONObj::opREGEX: {
            return _parseRegexDocument(name, context);
        }

        case BSONObj::opELEM_MATCH:
            return _parseElemMatch(name, e, level);

        case BSONObj::opALL:
            return _parseAll(name, e, level);

        case BSONObj::opWITHIN:
        case BSONObj::opGEO_INTERSECTS:
            return expressionParserGeoCallback(name, x, context);
    }

    return {Status(ErrorCodes::BadValue,
                   mongoutils::str::stream() << "not handled: " << e.fieldName())};
}

StatusWithMatchExpression MatchExpressionParser::_parse(const BSONObj& obj, int level) {
    if (level > kMaximumTreeDepth) {
        mongoutils::str::stream ss;
        ss << "exceeded maximum query tree depth of " << kMaximumTreeDepth << " at "
           << obj.toString();
        return {Status(ErrorCodes::BadValue, ss)};
    }

    std::unique_ptr<AndMatchExpression> root = stdx::make_unique<AndMatchExpression>();

    bool topLevel = (level == 0);
    level++;

    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement e = i.next();
        if (e.fieldName()[0] == '$') {
            const char* rest = e.fieldName() + 1;

            // TODO: optimize if block?
            if (mongoutils::str::equals("or", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "$or needs an array")};
                std::unique_ptr<OrMatchExpression> temp = stdx::make_unique<OrMatchExpression>();
                Status s = _parseTreeList(e.Obj(), temp.get(), level);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("and", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "and needs an array")};
                std::unique_ptr<AndMatchExpression> temp = stdx::make_unique<AndMatchExpression>();
                Status s = _parseTreeList(e.Obj(), temp.get(), level);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("nor", rest)) {
                if (e.type() != Array)
                    return {Status(ErrorCodes::BadValue, "and needs an array")};
                std::unique_ptr<NorMatchExpression> temp = stdx::make_unique<NorMatchExpression>();
                Status s = _parseTreeList(e.Obj(), temp.get(), level);
                if (!s.isOK())
                    return s;
                root->add(temp.release());
            } else if (mongoutils::str::equals("atomic", rest) ||
                       mongoutils::str::equals("isolated", rest)) {
                if (!topLevel)
                    return {Status(ErrorCodes::BadValue,
                                   "$atomic/$isolated has to be at the top level")};
                if (e.trueValue())
                    root->add(new AtomicMatchExpression());
            } else if (mongoutils::str::equals("where", rest)) {
                StatusWithMatchExpression s = _whereCallback->parseWhere(e);
                if (!s.isOK())
                    return s;
                root->add(s.getValue().release());
            } else if (mongoutils::str::equals("text", rest)) {
                if (e.type() != Object) {
                    return {Status(ErrorCodes::BadValue, "$text expects an object")};
                }
                StatusWithMatchExpression s = expressionParserTextCallback(e.Obj());
                if (!s.isOK()) {
                    return s;
                }
                root->add(s.getValue().release());
            } else if (mongoutils::str::equals("comment", rest)) {
            } else if (mongoutils::str::equals("ref", rest) ||
                       mongoutils::str::equals("id", rest) || mongoutils::str::equals("db", rest)) {
                // DBRef fields.
                std::unique_ptr<ComparisonMatchExpression> eq =
                    stdx::make_unique<EqualityMatchExpression>();
                Status s = eq->init(e.fieldName(), e);
                if (!s.isOK())
                    return s;

                root->add(eq.release());
            } else {
                return {Status(ErrorCodes::BadValue,
                               mongoutils::str::stream()
                                   << "unknown top level operator: " << e.fieldName())};
            }

            continue;
        }

        if (_isExpressionDocument(e, false)) {
            Status s = _parseSub(e.fieldName(), e.Obj(), root.get(), level);
            if (!s.isOK())
                return s;
            continue;
        }

        if (e.type() == RegEx) {
            StatusWithMatchExpression result = _parseRegexElement(e.fieldName(), e);
            if (!result.isOK())
                return result;
            root->add(result.getValue().release());
            continue;
        }

        std::unique_ptr<ComparisonMatchExpression> eq =
            stdx::make_unique<EqualityMatchExpression>();
        Status s = eq->init(e.fieldName(), e);
        if (!s.isOK())
            return s;

        root->add(eq.release());
    }

    if (root->numChildren() == 1) {
        std::unique_ptr<MatchExpression> real(root->getChild(0));
        root->clearAndRelease();
        return {std::move(real)};
    }

    return {std::move(root)};
}

Status MatchExpressionParser::_parseSub(const char* name,
                                        const BSONObj& sub,
                                        AndMatchExpression* root,
                                        int level) {
    // The one exception to {field : {fully contained argument} } is, of course, geo.  Example:
    // sub == { field : {$near[Sphere]: [0,0], $maxDistance: 1000, $minDistance: 10 } }
    // We peek inside of 'sub' to see if it's possibly a $near.  If so, we can't iterate over
    // its subfields and parse them one at a time (there is no $maxDistance without $near), so
    // we hand the entire object over to the geo parsing routines.

    if (level > kMaximumTreeDepth) {
        mongoutils::str::stream ss;
        ss << "exceeded maximum query tree depth of " << kMaximumTreeDepth << " at "
           << sub.toString();
        return Status(ErrorCodes::BadValue, ss);
    }

    level++;

    BSONObjIterator geoIt(sub);
    if (geoIt.more()) {
        BSONElement firstElt = geoIt.next();
        if (firstElt.isABSONObj()) {
            const char* fieldName = firstElt.fieldName();
            // TODO: Having these $fields here isn't ideal but we don't want to pull in anything
            // from db/geo at this point, since it may not actually be linked in...
            if (mongoutils::str::equals(fieldName, "$near") ||
                mongoutils::str::equals(fieldName, "$nearSphere") ||
                mongoutils::str::equals(fieldName, "$geoNear") ||
                mongoutils::str::equals(fieldName, "$maxDistance") ||
                mongoutils::str::equals(fieldName, "$minDistance")) {
                StatusWithMatchExpression s =
                    expressionParserGeoCallback(name, firstElt.getGtLtOp(), sub);
                if (s.isOK()) {
                    root->add(s.getValue().release());
                }

                // Propagate geo parsing result to caller.
                return s.getStatus();
            }
        }
    }

    BSONObjIterator j(sub);
    while (j.more()) {
        BSONElement deep = j.next();

        StatusWithMatchExpression s = _parseSubField(sub, root, name, deep, level);
        if (!s.isOK())
            return s.getStatus();

        if (s.getValue())
            root->add(s.getValue().release());
    }

    return Status::OK();
}

bool MatchExpressionParser::_isExpressionDocument(const BSONElement& e, bool allowIncompleteDBRef) {
    if (e.type() != Object)
        return false;

    BSONObj o = e.Obj();
    if (o.isEmpty())
        return false;

    const char* name = o.firstElement().fieldName();
    if (name[0] != '$')
        return false;

    if (_isDBRefDocument(o, allowIncompleteDBRef)) {
        return false;
    }

    return true;
}

/**
 * DBRef fields are ordered in the collection.
 * In the query, we consider an embedded object a query on
 * a DBRef as long as it contains $ref and $id.
 * Required fields: $ref and $id (if incomplete DBRefs are not allowed)
 *
 * If incomplete DBRefs are allowed, we accept the BSON object as long as it
 * contains $ref, $id or $db.
 *
 * Field names are checked but not field types.
 */
bool MatchExpressionParser::_isDBRefDocument(const BSONObj& obj, bool allowIncompleteDBRef) {
    bool hasRef = false;
    bool hasID = false;
    bool hasDB = false;

    BSONObjIterator i(obj);
    while (i.more() && !(hasRef && hasID)) {
        BSONElement element = i.next();
        const char* fieldName = element.fieldName();
        // $ref
        if (!hasRef && mongoutils::str::equals("$ref", fieldName)) {
            hasRef = true;
        }
        // $id
        else if (!hasID && mongoutils::str::equals("$id", fieldName)) {
            hasID = true;
        }
        // $db
        else if (!hasDB && mongoutils::str::equals("$db", fieldName)) {
            hasDB = true;
        }
    }

    if (allowIncompleteDBRef) {
        return hasRef || hasID || hasDB;
    }

    return hasRef && hasID;
}

StatusWithMatchExpression MatchExpressionParser::_parseMOD(const char* name, const BSONElement& e) {
    if (e.type() != Array)
        return {Status(ErrorCodes::BadValue, "malformed mod, needs to be an array")};

    BSONObjIterator i(e.Obj());

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    BSONElement d = i.next();
    if (!d.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, divisor not a number")};

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    BSONElement r = i.next();
    if (!d.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, remainder not a number")};

    if (i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, too many elements")};

    std::unique_ptr<ModMatchExpression> temp = stdx::make_unique<ModMatchExpression>();
    Status s = temp->init(name, d.numberInt(), r.numberInt());
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseRegexElement(const char* name,
                                                                    const BSONElement& e) {
    if (e.type() != RegEx)
        return {Status(ErrorCodes::BadValue, "not a regex")};

    std::unique_ptr<RegexMatchExpression> temp = stdx::make_unique<RegexMatchExpression>();
    Status s = temp->init(name, e.regex(), e.regexFlags());
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseRegexDocument(const char* name,
                                                                     const BSONObj& doc) {
    string regex;
    string regexOptions;

    BSONObjIterator i(doc);
    while (i.more()) {
        BSONElement e = i.next();
        switch (e.getGtLtOp()) {
            case BSONObj::opREGEX:
                if (e.type() == String) {
                    regex = e.String();
                } else if (e.type() == RegEx) {
                    regex = e.regex();
                    regexOptions = e.regexFlags();
                } else {
                    return {Status(ErrorCodes::BadValue, "$regex has to be a string")};
                }

                break;
            case BSONObj::opOPTIONS:
                if (e.type() != String)
                    return {Status(ErrorCodes::BadValue, "$options has to be a string")};
                regexOptions = e.String();
                break;
            default:
                break;
        }
    }

    std::unique_ptr<RegexMatchExpression> temp = stdx::make_unique<RegexMatchExpression>();
    Status s = temp->init(name, regex, regexOptions);
    if (!s.isOK())
        return s;
    return {std::move(temp)};
}

Status MatchExpressionParser::_parseArrayFilterEntries(ArrayFilterEntries* entries,
                                                       const BSONObj& theArray) {
    BSONObjIterator i(theArray);
    while (i.more()) {
        BSONElement e = i.next();

        // allow DBRefs but reject all fields with names starting wiht $
        if (_isExpressionDocument(e, false)) {
            return Status(ErrorCodes::BadValue, "cannot nest $ under $in");
        }

        if (e.type() == RegEx) {
            std::unique_ptr<RegexMatchExpression> r = stdx::make_unique<RegexMatchExpression>();
            Status s = r->init("", e);
            if (!s.isOK())
                return s;
            s = entries->addRegex(r.release());
            if (!s.isOK())
                return s;
        } else {
            Status s = entries->addEquality(e);
            if (!s.isOK())
                return s;
        }
    }
    return Status::OK();
}

StatusWithMatchExpression MatchExpressionParser::_parseType(const char* name,
                                                            const BSONElement& elt) {
    if (!elt.isNumber() && elt.type() != String) {
        return {Status(ErrorCodes::TypeMismatch, "argument to $type is not a number or a string")};
    }

    std::unique_ptr<TypeMatchExpression> temp = stdx::make_unique<TypeMatchExpression>();

    BSONType typeInt;

    // The element can be a number (the BSON type number) or a string representing the name
    // of the type.
    if (elt.isNumber()) {
        typeInt = (BSONType)elt.numberInt();
        if (elt.type() != NumberInt && typeInt != elt.number()) {
            typeInt = static_cast<BSONType>(-1);
        }
    } else {
        std::string typeAlias = elt.str();

        // Search the string-int map for the typeAlias (case-sensitive).
        std::unordered_map<std::string, BSONType>::const_iterator it =
            TypeMatchExpression::typeAliasMap.find(typeAlias);
        if (it == TypeMatchExpression::typeAliasMap.end()) {
            std::stringstream ss;
            ss << "unknown string alias for $type: " << typeAlias;
            return {Status(ErrorCodes::BadValue, ss.str())};
        }
        typeInt = it->second;
    }

    Status s = temp->init(name, typeInt);
    if (!s.isOK()) {
        return s;
    }

    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseElemMatch(const char* name,
                                                                 const BSONElement& e,
                                                                 int level) {
    if (e.type() != Object)
        return {Status(ErrorCodes::BadValue, "$elemMatch needs an Object")};

    BSONObj obj = e.Obj();

    // $elemMatch value case applies when the children all
    // work on the field 'name'.
    // This is the case when:
    //     1) the argument is an expression document; and
    //     2) expression is not a AND/NOR/OR logical operator. Children of
    //        these logical operators are initialized with field names.
    //     3) expression is not a WHERE operator. WHERE works on objects instead
    //        of specific field.
    bool isElemMatchValue = false;
    if (_isExpressionDocument(e, true)) {
        BSONObj o = e.Obj();
        BSONElement elt = o.firstElement();
        invariant(!elt.eoo());

        isElemMatchValue = !mongoutils::str::equals("$and", elt.fieldName()) &&
            !mongoutils::str::equals("$nor", elt.fieldName()) &&
            !mongoutils::str::equals("$or", elt.fieldName()) &&
            !mongoutils::str::equals("$where", elt.fieldName());
    }

    if (isElemMatchValue) {
        // value case

        AndMatchExpression theAnd;
        Status s = _parseSub("", obj, &theAnd, level);
        if (!s.isOK())
            return s;

        std::unique_ptr<ElemMatchValueMatchExpression> temp =
            stdx::make_unique<ElemMatchValueMatchExpression>();
        s = temp->init(name);
        if (!s.isOK())
            return s;

        for (size_t i = 0; i < theAnd.numChildren(); i++) {
            temp->add(theAnd.getChild(i));
        }
        theAnd.clearAndRelease();

        return {std::move(temp)};
    }

    // DBRef value case
    // A DBRef document under a $elemMatch should be treated as an object case
    // because it may contain non-DBRef fields in addition to $ref, $id and $db.

    // object case

    StatusWithMatchExpression subRaw = _parse(obj, level);
    if (!subRaw.isOK())
        return subRaw;
    std::unique_ptr<MatchExpression> sub = std::move(subRaw.getValue());

    // $where is not supported under $elemMatch because $where
    // applies to top-level document, not array elements in a field.
    if (hasNode(sub.get(), MatchExpression::WHERE)) {
        return {Status(ErrorCodes::BadValue, "$elemMatch cannot contain $where expression")};
    }

    std::unique_ptr<ElemMatchObjectMatchExpression> temp =
        stdx::make_unique<ElemMatchObjectMatchExpression>();
    Status status = temp->init(name, sub.release());
    if (!status.isOK())
        return status;

    return {std::move(temp)};
}

StatusWithMatchExpression MatchExpressionParser::_parseAll(const char* name,
                                                           const BSONElement& e,
                                                           int level) {
    if (e.type() != Array)
        return {Status(ErrorCodes::BadValue, "$all needs an array")};

    BSONObj arr = e.Obj();
    std::unique_ptr<AndMatchExpression> myAnd = stdx::make_unique<AndMatchExpression>();
    BSONObjIterator i(arr);

    if (arr.firstElement().type() == Object &&
        mongoutils::str::equals("$elemMatch",
                                arr.firstElement().Obj().firstElement().fieldName())) {
        // $all : [ { $elemMatch : {} } ... ]

        while (i.more()) {
            BSONElement hopefullyElemMatchElement = i.next();

            if (hopefullyElemMatchElement.type() != Object) {
                // $all : [ { $elemMatch : ... }, 5 ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            BSONObj hopefullyElemMatchObj = hopefullyElemMatchElement.Obj();
            if (!mongoutils::str::equals("$elemMatch",
                                         hopefullyElemMatchObj.firstElement().fieldName())) {
                // $all : [ { $elemMatch : ... }, { x : 5 } ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            StatusWithMatchExpression inner =
                _parseElemMatch(name, hopefullyElemMatchObj.firstElement(), level);
            if (!inner.isOK())
                return inner;
            myAnd->add(inner.getValue().release());
        }

        return {std::move(myAnd)};
    }

    while (i.more()) {
        BSONElement e = i.next();

        if (e.type() == RegEx) {
            std::unique_ptr<RegexMatchExpression> r = stdx::make_unique<RegexMatchExpression>();
            Status s = r->init(name, e);
            if (!s.isOK())
                return s;
            myAnd->add(r.release());
        } else if (e.type() == Object && e.Obj().firstElement().getGtLtOp(-1) != -1) {
            return {Status(ErrorCodes::BadValue, "no $ expressions in $all")};
        } else {
            std::unique_ptr<EqualityMatchExpression> x =
                stdx::make_unique<EqualityMatchExpression>();
            Status s = x->init(name, e);
            if (!s.isOK())
                return s;
            myAnd->add(x.release());
        }
    }

    if (myAnd->numChildren() == 0) {
        return {stdx::make_unique<FalseMatchExpression>()};
    }

    return {std::move(myAnd)};
}

StatusWithMatchExpression MatchExpressionParser::WhereCallback::parseWhere(
    const BSONElement& where) const {
    return {Status(ErrorCodes::NoWhereParseContext, "no context for parsing $where")};
}

// Geo
StatusWithMatchExpression expressionParserGeoCallbackDefault(const char* name,
                                                             int type,
                                                             const BSONObj& section) {
    return {Status(ErrorCodes::BadValue, "geo not linked in")};
}

MatchExpressionParserGeoCallback expressionParserGeoCallback = expressionParserGeoCallbackDefault;

// Text
StatusWithMatchExpression expressionParserTextCallbackDefault(const BSONObj& queryObj) {
    return {Status(ErrorCodes::BadValue, "$text not linked in")};
}

MatchExpressionParserTextCallback expressionParserTextCallback =
    expressionParserTextCallbackDefault;
}

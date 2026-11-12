#include "storage/storage_keyring.h"

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#define ELEZA_SECRET_SERVICE
#include <QtCore/QLibrary>
#endif // Q_OS_UNIX && !Q_OS_DARWIN

namespace Storage {
namespace {

Keyring *Override = nullptr;
bool Overridden = false;

#ifdef ELEZA_SECRET_SERVICE

// The parts of libsecret's (and GLib's) ABI that are used, as its headers
// declare them (libsecret/secret-schema.h, glib/gerror.h); stable since
// libsecret 0.7.
struct SecretSchemaAttribute {
	const char *name = nullptr;
	int type = 0; // SECRET_SCHEMA_ATTRIBUTE_STRING
};

struct SecretSchema {
	const char *name = nullptr;
	int flags = 0; // SECRET_SCHEMA_NONE
	SecretSchemaAttribute attributes[32];
	int reserved = 0;
	void *reserved1 = nullptr;
	void *reserved2 = nullptr;
	void *reserved3 = nullptr;
	void *reserved4 = nullptr;
	void *reserved5 = nullptr;
	void *reserved6 = nullptr;
	void *reserved7 = nullptr;
};

static_assert(sizeof(void*) != 8 || sizeof(SecretSchema) == 592);

struct GError {
	quint32 domain = 0;
	int code = 0;
	char *message = nullptr;
};

// Attribute name / value pairs follow, then a null.
using StoreFunction = int (*)(
	const SecretSchema *schema,
	const char *collection,
	const char *label,
	const char *password,
	void *cancellable,
	GError **error,
	...);
using LookupFunction = char *(*)(
	const SecretSchema *schema,
	void *cancellable,
	GError **error,
	...);
using ClearFunction = int (*)(
	const SecretSchema *schema,
	void *cancellable,
	GError **error,
	...);
using FreeFunction = void (*)(char *password);
using ErrorFreeFunction = void (*)(GError *error);

constexpr auto kAttribute = "blob";

class SecretService final : public Keyring {
public:
	// Null when the library or a service is not there.
	[[nodiscard]] static std::unique_ptr<SecretService> Load() {
		auto result = std::unique_ptr<SecretService>(new SecretService());
		if (!result->resolve()) {
			qWarning(
				"Storage: libsecret-1.so.0 is not available; the session and "
				"the device key are kept in owner-only files.");
			return nullptr;
		}
		auto probe = QByteArray();
		if (result->lookup(u"probe"_s, probe) == Lookup::Unavailable) {
			qWarning(
				"Storage: no Secret Service answers; the session and the "
				"device key are kept in owner-only files.");
			return nullptr;
		}
		return result;
	}

	bool store(
			const QString &id,
			const QString &label,
			const QByteArray &secret) override {
		auto error = (GError*)nullptr;
		const auto stored = _store(
			&_schema,
			nullptr, // the default collection
			label.toUtf8().constData(),
			secret.constData(),
			nullptr,
			&error,
			kAttribute,
			id.toUtf8().constData(),
			nullptr);
		return check(error, "store") && stored;
	}

	Lookup lookup(const QString &id, QByteArray &secret) override {
		auto error = (GError*)nullptr;
		const auto found = _lookup(
			&_schema,
			nullptr,
			&error,
			kAttribute,
			id.toUtf8().constData(),
			nullptr);
		if (!check(error, "lookup")) {
			if (found) {
				_free(found);
			}
			return Lookup::Unavailable;
		} else if (!found) {
			return Lookup::Missing;
		}
		secret = QByteArray(found);
		_free(found); // secret_password_free() wipes it first
		return Lookup::Found;
	}

	void remove(const QString &id) override {
		auto error = (GError*)nullptr;
		_clear(&_schema, nullptr, &error, kAttribute, id.toUtf8().constData(), nullptr);
		check(error, "clear");
	}

private:
	SecretService() : _library(u"secret-1"_s, 0) {
		_schema.name = "me.eleza.desktop.SecureBlob";
		_schema.attributes[0].name = kAttribute;
	}

	[[nodiscard]] bool resolve() {
		if (!_library.load()) {
			return false;
		}
		_store = reinterpret_cast<StoreFunction>(
			_library.resolve("secret_password_store_sync"));
		_lookup = reinterpret_cast<LookupFunction>(
			_library.resolve("secret_password_lookup_sync"));
		_clear = reinterpret_cast<ClearFunction>(
			_library.resolve("secret_password_clear_sync"));
		_free = reinterpret_cast<FreeFunction>(
			_library.resolve("secret_password_free"));
		// GLib is a dependency of libsecret: found through its handle.
		_errorFree = reinterpret_cast<ErrorFreeFunction>(
			_library.resolve("g_error_free"));
		return _store && _lookup && _clear && _free && _errorFree;
	}

	// True when there was no error (which is freed).
	bool check(GError *error, const char *what) {
		if (!error) {
			return true;
		}
		qWarning(
			"Storage: Secret Service %s failed: %s",
			what,
			error->message ? error->message : "?");
		_errorFree(error);
		return false;
	}

	QLibrary _library;
	SecretSchema _schema;
	StoreFunction _store = nullptr;
	LookupFunction _lookup = nullptr;
	ClearFunction _clear = nullptr;
	FreeFunction _free = nullptr;
	ErrorFreeFunction _errorFree = nullptr;

};

#endif // ELEZA_SECRET_SERVICE

} // namespace

Keyring *Keyring::Default() {
	if (Overridden) {
		return Override;
	}
#ifdef ELEZA_SECRET_SERVICE
	static const auto Service = [] {
		return (qEnvironmentVariable("ELEZA_SECRET_SERVICE") == u"0"_s)
			? std::unique_ptr<SecretService>()
			: SecretService::Load();
	}();
	return Service.get();
#else // ELEZA_SECRET_SERVICE
	return nullptr;
#endif // !ELEZA_SECRET_SERVICE
}

void Keyring::SetDefaultForTests(Keyring *keyring) {
	Overridden = true;
	Override = keyring;
}

} // namespace Storage
